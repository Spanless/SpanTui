#include "terminal/core/terminal_core_worker.h"
#include "terminal/shared/terminal_frame_store.h"
#include "terminal/shared/terminal_output_queue.h"
#include <QTest>

#include <algorithm>

using namespace span::terminal;

namespace {
QString frameText(const Frame &frame) {
    QString result;
    for (const Line &line : frame.lines) {
        for (const Cell &cell : line.cells) {
            if (!cell.continuation) {
                result += cell.text.isEmpty() ? QStringLiteral(" ") : cell.text;
            }
        }
        result += QLatin1Char('\n');
    }
    return result;
}

// "line <n>\r\n" for n in [0, count), with " needle" after line `needleLine`.
QByteArray numberedLines(int count, int needleLine)
{
    QByteArray bytes;
    for (int index = 0; index < count; ++index)
        bytes += "line " + QByteArray::number(index)
            + (index == needleLine ? QByteArrayLiteral(" needle") : QByteArray()) + "\r\n";
    return bytes;
}
}

class TerminalCoreWorkerTest final : public QObject {
    Q_OBJECT

private slots:
    void searchesHistoryOffThreadAndRevealsLines();
    void keepsUserEchoCursorVisible();
    void forceEndsStalledSynchronizedUpdateSoLaterFramesPublish();
};

void TerminalCoreWorkerTest::searchesHistoryOffThreadAndRevealsLines() {
    auto queue = std::make_shared<TerminalOutputQueue>();
    QVERIFY(queue->open() || queue->size() == 0);
    auto store = std::make_shared<TerminalFrameStore>();
    TerminalCoreWorker worker(QStringLiteral("search"), 20, 4, 100, queue, store);
    worker.initialize();
    worker.setRenderConsumer(true, 60);

    queue->append(numberedLines(12, 2));
    worker.drainOutput();
    QTRY_VERIFY_WITH_TIMEOUT(frameText(store->latest().frame).contains(QStringLiteral("line 11")), 2000);

    const quint64 jumpBefore = store->latest().viewportJump;
    QVERIFY(store->latest().frame.scrollbar.atBottom());

    // Matching ignores ASCII case. The first count reveals the newest match:
    // the row is in history, so the core scrolls to it and says so.
    worker.setSearchNeedle(QStringLiteral("NEEDLE"));
    QTRY_VERIFY_WITH_TIMEOUT(store->latest().search.complete && store->latest().search.total == 1
                                 && store->latest().viewportJump != jumpBefore,
                             5000);
    const TerminalFrameSnapshot found = store->latest();
    QCOMPARE(found.search.needle, QStringLiteral("NEEDLE"));
    QCOMPARE(found.search.selected, 0);
    QVERIFY(!found.frame.scrollbar.atBottom());
    const auto current = std::find_if(found.frame.searchMatches.cbegin(), found.frame.searchMatches.cend(),
                                      [](const TerminalHighlight &match) { return match.current; });
    QVERIFY(current != found.frame.searchMatches.cend());
    QCOMPARE(current->startColumn, 7);
    QCOMPARE(current->endColumn, 13);
    const int viewportLine = current->line - found.frame.viewportRow;
    QVERIFY(viewportLine >= 0 && viewportLine < found.frame.rows);
    Frame matched;
    matched.lines = {found.frame.lines.at(current->line)};
    QVERIFY(frameText(matched).contains(QStringLiteral("line 2 needle")));

    // An empty needle ends the search and its highlights.
    worker.setSearchNeedle(QString());
    QTRY_VERIFY_WITH_TIMEOUT(store->latest().search.needle.isEmpty() && store->latest().frame.searchMatches.isEmpty(),
                             2000);
    QCOMPARE(store->latest().search.total, 0);
    worker.shutdown();
}

void TerminalCoreWorkerTest::keepsUserEchoCursorVisible() {
    auto queue = std::make_shared<TerminalOutputQueue>();
    QVERIFY(queue->open() || queue->size() == 0);
    auto store = std::make_shared<TerminalFrameStore>();
    TerminalCoreWorker worker(QStringLiteral("cursor-user-echo"), 40, 8, 100,
                              queue, store);
    worker.initialize();
    worker.setRenderConsumer(true, 60);

    queue->append(QByteArrayLiteral(
        "\x1b[?2026h\x1b[2J\x1b[2;2H\x1b[?2026l"));
    worker.drainOutput();
    QTRY_VERIFY_WITH_TIMEOUT(store->latest().frame.cursor.visible, 1000);

    worker.sendKeyStroke(TerminalKeyStroke::character(QStringLiteral("x"), Qt::NoModifier,
                    TerminalKeyEvent::Press));
    queue->append(QByteArrayLiteral("x"));
    worker.drainOutput();
    QTRY_COMPARE_WITH_TIMEOUT(store->latest().frame.cursor.column, 2, 1000);
    QVERIFY(store->latest().frame.cursor.visible);
    worker.shutdown();
}

// A program that opens a synchronized update and never closes it must not
// freeze the screen.
void TerminalCoreWorkerTest::forceEndsStalledSynchronizedUpdateSoLaterFramesPublish() {
    auto queue = std::make_shared<TerminalOutputQueue>();
    auto store = std::make_shared<TerminalFrameStore>();
    QVERIFY(queue->open() || queue->size() == 0);

    TerminalCoreWorker worker(QStringLiteral("stalled-sync"), 20, 4, 64, queue, store);
    worker.initialize();
    worker.setRenderConsumer(true, 60);
    QTRY_VERIFY_WITH_TIMEOUT(store->latest().sequence > 0, 2000);
    const quint64 baseline = store->latest().sequence;
    QVERIFY(store->acknowledgeNotification(baseline) ==
            FrameNotificationDisposition::Released);

    const QByteArray braille = QString::fromUtf8(u8"⣿").toUtf8();
    // Open a synchronized update and write cells, but never send 2026l.
    queue->append(QByteArrayLiteral("\x1b[?2026h\x1b[H\x1b[38;2;10;20;30m") + braille);
    worker.drainOutput();

    // Once the incomplete frame goes inactive, force-end and publish it.
    QTRY_VERIFY_WITH_TIMEOUT(store->latest().sequence > baseline, 2000);
    QCOMPARE(store->latest().frame.lines.first().cells.first().style.foreground,
             QColor(10, 20, 30));

    // The producer may still send the matching end after the watchdog has
    // published the stalled frame. That delayed marker completes recovery and
    // must not be diagnosed as a new unmatched end.
    QTest::failOnWarning("PTY sent synchronized-update end without a begin");
    queue->append(QByteArrayLiteral("\x1b[?2026l"));
    worker.drainOutput();

    // A later complete frame must still publish after the forced end.
    const quint64 afterForce = store->latest().sequence;
    QVERIFY(store->acknowledgeNotification(afterForce) ==
            FrameNotificationDisposition::Released);
    queue->append(QByteArrayLiteral("\x1b[?2026h\x1b[H\x1b[38;2;200;100;50m") + braille +
                  QByteArrayLiteral("\x1b[?2026l"));
    worker.drainOutput();
    QTRY_VERIFY_WITH_TIMEOUT(store->latest().sequence > afterForce, 2000);
    QCOMPARE(store->latest().frame.lines.first().cells.first().style.foreground,
             QColor(200, 100, 50));
    worker.shutdown();
}

QTEST_MAIN(TerminalCoreWorkerTest)
#include "terminal_core_worker_test.moc"
