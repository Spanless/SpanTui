#include "terminal/core/terminal_core_worker.h"
#include "terminal/shared/terminal_frame_store.h"
#include "terminal/shared/terminal_output_queue.h"
#include "terminal/terminal_constants.h"
#include "property_check.h"
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QScopeGuard>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>
#include <QThread>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <thread>

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

void stopWorker(QThread *thread, TerminalCoreWorker *worker) {
    if (thread->isRunning()) {
        QThread *ownerThread = QThread::currentThread();
        QMetaObject::invokeMethod(worker, [worker, ownerThread]() {
            worker->shutdown();
            worker->moveToThread(ownerThread);
        }, Qt::BlockingQueuedConnection);
        thread->quit();
        thread->wait(30000);
    }
    delete worker;
}

void attachQueueWake(const std::shared_ptr<TerminalOutputQueue> &queue,
                     TerminalCoreWorker *worker) {
    queue->setWakeCallback([worker]() {
        QMetaObject::invokeMethod(worker,
                                  &TerminalCoreWorker::drainOutput,
                                  Qt::QueuedConnection);
    });
}

// The GUI's acknowledgement of whatever latest() holds; the store rejects
// sequence 0 itself.
void acknowledgeLatest(TerminalFrameStore &store)
{
    store.acknowledgeNotification(store.latest().sequence);
}

// `unit` repeated until the result is at least `size` bytes.
QByteArray repeatedPast(const QByteArray &unit, qsizetype size)
{
    QByteArray bytes;
    bytes.reserve(size + unit.size());
    while (bytes.size() < size) bytes += unit;
    return bytes;
}

void closeAndJoin(TerminalOutputQueue &queue, std::thread &producer)
{
    queue.close();
    if (producer.joinable()) producer.join();
}

// Prints fifty history lines in ten chunks `waitMs` apart, as a resize-driven
// transcript reprint does, resizing to 50 columns before chunk
// `resizeBeforeChunk` (-1: never). True when no frame was published past
// `before` while it ran.
bool reprintStaysUnpublished(TerminalCoreWorker &worker, TerminalOutputQueue &queue, TerminalFrameStore &store,
                             quint64 before, int resizeBeforeChunk, int waitMs)
{
    for (int chunk = 0; chunk < 10; ++chunk) {
        if (chunk == resizeBeforeChunk) worker.resize(50, 8, 7);
        QByteArray lines;
        for (int line = 0; line < 5; ++line) lines += "history-" + QByteArray::number(chunk * 5 + line) + "\r\n";
        queue.append(lines);
        worker.drainOutput();
        QTest::qWait(waitMs);
        if (store.latest().sequence != before) return false;
    }
    return true;
}

// `\r<prefix><n>\x1b[K` for n in [0, count): one line rewritten count times.
QByteArray numberedRewrites(const QByteArray &prefix, int count)
{
    QByteArray bytes;
    for (int index = 0; index < count; ++index) bytes += "\r" + prefix + QByteArray::number(index) + "\x1b[K";
    return bytes;
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

// Cursor-addressed "line-<row>" on rows 1..rows.
QByteArray addressedRows(int rows)
{
    QByteArray bytes;
    for (int row = 1; row <= rows; ++row)
        bytes += "\x1b[" + QByteArray::number(row) + ";1Hline-" + QByteArray::number(row);
    return bytes;
}

// Pumps events continuously until `done` holds or `clock` passes `timeoutMs`.
// QTest::qWait sleeps in 10ms slices that Windows rounds up to its timer tick,
// which can starve the worker's timer chain.
template <typename Done> void pumpUntil(Done done, const QElapsedTimer &clock, qint64 timeoutMs)
{
    while (!done() && clock.elapsed() < timeoutMs) QCoreApplication::processEvents(QEventLoop::AllEvents, 1);
}

// Sends `frames` synchronized status redraws that hide the cursor while they
// paint and restore it to row 6, column 4. Counts the redraws that published a
// frame with the cursor visible there.
int redrawsRestoringCursor(TerminalCoreWorker &worker, TerminalOutputQueue &queue, TerminalFrameStore &store,
                           int frames)
{
    int restored = 0;
    for (int frame = 0; frame < frames; ++frame) {
        const quint64 previous = store.latest().sequence;
        queue.append(QByteArrayLiteral("\x1b[?2026h\x1b[?25l\x1b[2;2HWorking"
                                       "\x1b[10;3Hgpt-5.6-sol\x1b[?25h\x1b[6;4H\x1b[?2026l"));
        worker.drainOutput();
        const bool published = QTest::qWaitFor([&]() { return store.latest().sequence > previous; }, 1000);
        const auto cursor = store.latest().frame.cursor;
        restored += published && cursor.visible && cursor.row == 5 && cursor.column == 3;
    }
    return restored;
}

struct ParsedStream {
    QByteArray bytes;
    int begins = 0;
    int ends = 0;
    bool operator==(const ParsedStream &other) const
    {
        return bytes == other.bytes && begins == other.begins && ends == other.ends;
    }
};

// Feeds `pieces` to one parser in order, then flushes it.
ParsedStream parsePieces(const QList<QByteArrayView> &pieces)
{
    SynchronizedUpdateParser parser;
    ParsedStream parsed;
    for (QByteArrayView piece : pieces) {
        for (const auto &segment : parser.push(piece)) {
            parsed.bytes += segment.bytes;
            parsed.begins += segment.boundaryAfter == SynchronizedUpdateBoundary::Begin;
            parsed.ends += segment.boundaryAfter == SynchronizedUpdateBoundary::End;
        }
    }
    parsed.bytes += parser.flushPending();
    return parsed;
}

ParsedStream parseBytewise(const QByteArray &stream)
{
    QList<QByteArrayView> pieces;
    for (qsizetype index = 0; index < stream.size(); ++index) pieces.append(QByteArrayView(stream).sliced(index, 1));
    return parsePieces(pieces);
}

// First split point at which pushing `stream` in two pieces parses
// differently from pushing it whole, or -1.
qsizetype firstDivergentSplit(const QByteArray &stream)
{
    const ParsedStream whole = parsePieces({stream});
    for (qsizetype split = 1; split < stream.size(); ++split) {
        if (!(parsePieces({stream.first(split), stream.sliced(split)}) == whole)) return split;
    }
    return -1;
}

struct SplitStream {
    QByteArray bytes;
    QList<qsizetype> cuts;
};

// Streams of synchronized-update markers, their near misses and plain text,
// cut at random points.
SplitStream randomSplitStream(QRandomGenerator &random)
{
    static const QList<QByteArray> alphabet{QByteArrayLiteral("\x1b[?2026h"),
                                            QByteArrayLiteral("\x1b[?2026l"),
                                            QByteArrayLiteral("\x1b[?25;2026h"),
                                            QByteArrayLiteral("\x1b[?2026;25l"),
                                            QByteArrayLiteral("\x1b[?2025h"),
                                            QByteArrayLiteral("\x1b["),
                                            QByteArrayLiteral("\x1b"),
                                            QByteArrayLiteral("?2026"),
                                            QByteArrayLiteral(";"),
                                            QByteArrayLiteral("h"),
                                            QByteArrayLiteral("l"),
                                            QByteArrayLiteral("text"),
                                            QByteArrayLiteral("\r\n")};
    SplitStream stream;
    stream.bytes = span::tests::tokenString(random, alphabet, 24);
    stream.cuts = span::tests::randomCuts(random, stream.bytes.size(), 8);
    return stream;
}

bool parsesLikeWhole(const SplitStream &stream)
{
    const QList<QByteArray> pieces = span::tests::piecesAt(stream.bytes, stream.cuts);
    const QList<QByteArrayView> views(pieces.cbegin(), pieces.cend());
    return parsePieces(views) == parsePieces({stream.bytes});
}

// The parser only marks boundaries; every byte it was given comes out.
bool keepsEveryByte(const SplitStream &stream)
{
    const QList<QByteArray> pieces = span::tests::piecesAt(stream.bytes, stream.cuts);
    const QList<QByteArrayView> views(pieces.cbegin(), pieces.cend());
    return parsePieces(views).bytes == stream.bytes;
}

QString describeSplit(const SplitStream &stream)
{
    QStringList cuts;
    for (qsizetype cut : stream.cuts) cuts.append(QString::number(cut));
    return QStringLiteral("bytes [%1] cuts [%2]").arg(span::tests::hexOf(stream.bytes), cuts.join(u' '));
}

// Frames published past `*lastSequence` since the previous call, each
// acknowledged as the GUI would.
int collectPublished(TerminalFrameStore &store, quint64 *lastSequence)
{
    const quint64 sequence = store.latest().sequence;
    if (sequence <= *lastSequence) return 0;
    *lastSequence = sequence;
    acknowledgeLatest(store);
    return 1;
}

// Rewrites one line through a worker on another thread for `durationMs`, far
// faster than any render rate; returns how many frames were published.
int pumpLineRewrites(TerminalOutputQueue &queue, TerminalCoreWorker *worker, TerminalFrameStore &store, int durationMs)
{
    QElapsedTimer clock;
    clock.start();
    quint64 lastSequence = store.latest().sequence;
    int published = 0;
    int update = 0;
    while (clock.elapsed() < durationMs) {
        queue.append(QByteArrayLiteral("\rrate-") + QByteArray::number(update++) + QByteArrayLiteral("\x1b[K"));
        QMetaObject::invokeMethod(worker, &TerminalCoreWorker::drainOutput, Qt::QueuedConnection);
        published += collectPublished(store, &lastSequence);
        QTest::qWait(1);
    }
    return published;
}

// Recolors two rows every 5 ms without synchronized updates for
// `durationMs`, then drains the rest; returns how many frames were published.
int pumpTwoRowAnimation(TerminalOutputQueue &queue, TerminalCoreWorker &worker, TerminalFrameStore &store,
                        int durationMs)
{
    QElapsedTimer wall;
    wall.start();
    quint64 lastSequence = store.latest().sequence;
    int published = 0;
    for (int frame = 0; wall.elapsed() < durationMs; ++frame) {
        const QByteArray shade = QByteArray::number(40 + (frame % 180));
        queue.append(QByteArrayLiteral("\x1b[H\x1b[38;2;") + shade + QByteArrayLiteral(";80;160mrow-a-")
                     + QByteArray::number(frame) + QByteArrayLiteral("\x1b[K\x1b[2;1H\x1b[38;2;160;") + shade
                     + QByteArrayLiteral(";80mrow-b-") + QByteArray::number(frame) + QByteArrayLiteral("\x1b[K"));
        worker.drainOutput();
        published += collectPublished(store, &lastSequence);
        QTest::qWait(5);
    }
    // Drain residual bytes and collect a final budget frame.
    QTest::qWaitFor([&queue]() { return queue.size() == 0; }, 2000);
    QTest::qWait(80);
    return published + collectPublished(store, &lastSequence);
}
}

class TerminalCoreWorkerTest final : public QObject {
    Q_OBJECT

private slots:
    void publishesStableCursorWithoutFrameAcknowledgement();
    void searchesHistoryOffThreadAndRevealsLines();
    void collapsesFragmentedRatatuiDrawTrajectory();
    void publishesSynchronizedCursorWithItsExactFrame();
    void suppressesTransientHideShowDuringSynchronizedRedraw();
    void keepsUserEchoCursorVisible();
    void parsesSynchronizedUpdatesAcrossEverySplit();
    void parsesRandomStreamsAlikeAcrossRandomSplits();
    void passesRandomStreamsThroughVerbatim();
    void publishesIntermediateFramesAndObservesSynchronizedUpdates();
    void suppressesHiddenFramesAndCapsPublicationRate();
    void resetsAllProtocolStateForANewTransportEpoch();
    void coalescesTruecolorShimmerWhileNotificationPending();
    void forceEndsStalledSynchronizedUpdateSoLaterFramesPublish();
    void acceptsIdempotentSynchronizedUpdateModeChanges();
    void publishesUnsynchronizedRedrawAtSelectedFrameRate();
    void keepsSingleCellTypingResponsive();
    void publishesContinuousUnsynchronizedMultiRowAnimation();
    void presentsPostResizeRepaintAsOneSettledFrame();
    void presentsScrollbackEraseReprintAsOneFrame();
    void holdsPartialProducerWriteUntilItsEnd();
};

void TerminalCoreWorkerTest::presentsPostResizeRepaintAsOneSettledFrame() {
    auto queue = std::make_shared<TerminalOutputQueue>();
    auto store = std::make_shared<TerminalFrameStore>();
    QVERIFY(queue->open() || queue->size() == 0);

    TerminalCoreWorker worker(QStringLiteral("resize-repaint-settle"), 40, 8, 256,
                              queue, store);
    worker.initialize();
    worker.setRenderConsumer(true, 60);
    QTRY_VERIFY_WITH_TIMEOUT(store->latest().sequence > 0, 2000);
    store->acknowledgeNotification(store->latest().sequence);

    // The reflowed grid itself is presented immediately under the new epoch.
    worker.resize(40, 12, 1);
    QTRY_COMPARE_WITH_TIMEOUT(store->latest().viewportEpoch, quint64(1), 500);
    const quint64 resized = store->latest().sequence;
    store->acknowledgeNotification(resized);

    // A TUI answers the resize by clearing and reprinting its transcript in
    // chunks. None of the scroll-through states may reach the store.
    queue->append(QByteArrayLiteral("\x1b[2J\x1b[H"));
    worker.drainOutput();
    QVERIFY(reprintStaysUnpublished(worker, *queue, *store, resized, -1, 5));
    queue->append(QByteArrayLiteral("prompt> "));
    worker.drainOutput();

    // Once the stream is quiet the final state is presented in one frame.
    QTRY_VERIFY_WITH_TIMEOUT(store->latest().sequence > resized, 500);
    const TerminalFrameSnapshot settled = store->latest();
    QCOMPARE(settled.sequence, resized + 1);
    QVERIFY(frameText(settled.frame).contains(QStringLiteral("history-49")));
    QVERIFY(frameText(settled.frame).contains(QStringLiteral("prompt> ")));
    // The return to the frame cadence after the settle window is
    // TerminalPresentationSchedulerTest::settlesRepaintAfterResize.
    worker.shutdown();
}

void TerminalCoreWorkerTest::presentsScrollbackEraseReprintAsOneFrame() {
    SynchronizedUpdateParser parser;
    const auto segments = parser.push(QByteArrayLiteral("a\x1b[2Jb\x1b[3Jc\x1b[?3J"));
    // Only ED3 is a boundary; ED2 and the private ?3J are ordinary bytes.
    QCOMPARE(segments.size(), 3);
    QCOMPARE(segments.at(0).boundaryAfter, SynchronizedUpdateBoundary::None);
    QCOMPARE(segments.at(1).boundaryAfter, SynchronizedUpdateBoundary::RedrawBegin);
    QCOMPARE(segments.at(1).bytes, QByteArrayLiteral("b\x1b[3J"));
    QCOMPARE(segments.at(2).boundaryAfter, SynchronizedUpdateBoundary::None);

    auto queue = std::make_shared<TerminalOutputQueue>();
    auto store = std::make_shared<TerminalFrameStore>();
    QVERIFY(queue->open() || queue->size() == 0);
    TerminalCoreWorker worker(QStringLiteral("ed3-reprint"), 40, 8, 256, queue, store);
    worker.initialize();
    worker.setRenderConsumer(true, 60);
    queue->append(QByteArrayLiteral("before"));
    worker.drainOutput();
    QTRY_VERIFY_WITH_TIMEOUT(frameText(store->latest().frame).contains(QStringLiteral("before")),
                             2000);
    const quint64 before = store->latest().sequence;
    store->acknowledgeNotification(before);

    // omp answers every resize with ED2+ED3 and its whole transcript, with no
    // 2026 transaction around it. A resize that lands
    // mid-reprint (a window drag) must not present the half-printed history.
    queue->append(QByteArrayLiteral("\x1b[H\x1b[2J\x1b[3J\x1b[H\x1b[?2026l"));
    worker.drainOutput();
    QVERIFY(reprintStaysUnpublished(worker, *queue, *store, before, 5, 10));
    queue->append(QByteArrayLiteral("prompt> "));
    worker.drainOutput();

    QTRY_VERIFY_WITH_TIMEOUT(store->latest().sequence > before, 500);
    const TerminalFrameSnapshot settled = store->latest();
    QCOMPARE(settled.sequence, before + 1);
    QCOMPARE(settled.viewportEpoch, quint64(7));
    QCOMPARE(settled.frame.columns, 50);
    QVERIFY(frameText(settled.frame).contains(QStringLiteral("history-49")));
    QVERIFY(frameText(settled.frame).contains(QStringLiteral("prompt> ")));
    store->acknowledgeNotification(settled.sequence);

    // Ordinary output after the reprint publishes on the frame cadence again.
    queue->append(QByteArrayLiteral("x"));
    worker.drainOutput();
    QTRY_VERIFY_WITH_TIMEOUT(store->latest().sequence > settled.sequence, 100);
    worker.shutdown();
}

void TerminalCoreWorkerTest::resetsAllProtocolStateForANewTransportEpoch() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString sessionId = QStringLiteral("transport-epoch-reset-test");

    auto queue = std::make_shared<TerminalOutputQueue>();
    QVERIFY(queue->open() || queue->size() == 0);
    auto store = std::make_shared<TerminalFrameStore>();
    TerminalCoreWorker worker(sessionId, 20, 4, 100, queue, store);
    QSignalSpy output(&worker, &TerminalCoreWorker::outputRequested);
    worker.initialize();

    queue->append(QByteArrayLiteral("old-primary\r\n\x1b[?1049h\x1b[Halt-screen\x1b[=3u"));
    worker.drainOutput();
    output.clear();
    worker.sendKeyStroke(TerminalKeyStroke::character(QStringLiteral("a"), Qt::ControlModifier, TerminalKeyEvent::Press));
    // Kitty flags 3: Ctrl+A as CSI-u; a press omits its default event type.
    QCOMPARE(output.takeFirst().at(0).toByteArray(), QByteArrayLiteral("\x1b[97;5u"));

    worker.beginTransportEpoch();
    const TerminalFrameSnapshot epochSnapshot = store->latest();
    QVERIFY(epochSnapshot.sequence > 0);
    // With no render consumer, latest() is materialized but no GUI
    // notification reservation is retained.
    QVERIFY(!store->notificationPending());
    // A new transport gets a new engine: the keyboard protocol, the
    // alternate screen and the history all start over.
    QVERIFY(epochSnapshot.transportEpoch > 0);
    worker.resize(160, 50);
    worker.resize(20, 4);
    worker.sendKeyStroke(TerminalKeyStroke::character(QStringLiteral("a"), Qt::ControlModifier, TerminalKeyEvent::Press));
    QCOMPARE(output.takeFirst().at(0).toByteArray(), QByteArrayLiteral("\x01"));

    const TerminalFrameSnapshot fresh = store->latest();
    QVERIFY(fresh.transportEpoch > 0);
    QVERIFY(!fresh.frame.alternateScreen);
    QCOMPARE(fresh.frame.scrollbar.total, qint64(fresh.frame.rows));
    QVERIFY(!frameText(fresh.frame).contains(QStringLiteral("old-primary")));
    QVERIFY(!frameText(fresh.frame).contains(QStringLiteral("alt-screen")));
    worker.shutdown();
}

void TerminalCoreWorkerTest::publishesStableCursorWithoutFrameAcknowledgement() {
    auto queue = std::make_shared<TerminalOutputQueue>();
    QVERIFY(queue->open() || queue->size() == 0);
    auto store = std::make_shared<TerminalFrameStore>();
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    TerminalCoreWorker worker(QStringLiteral("live-ime-cursor"), 40, 8, 100, queue, store);
    worker.initialize();
    worker.setRenderConsumer(true, 60);
    QTRY_VERIFY_WITH_TIMEOUT(store->latest().sequence > 0, 2000);

    // Leave the frame notification in flight, exactly as a busy GUI consumer
    // would. New state may coalesce into latest(), but the cursor has no side
    // channel and therefore can only appear inside that newer full snapshot.
    queue->append(QByteArrayLiteral(
        "\x1b[?2026h\x1b[2J\x1b[4;11Hrender-tail\x1b[?2026l"));
    worker.drainOutput();
    QTRY_VERIFY_WITH_TIMEOUT(store->latest().frame.cursor.row == 3, 2000);
    const TerminalFrameSnapshot renderFrame = store->latest();
    QVERIFY(store->notificationPending());
    QCOMPARE(renderFrame.frame.cursor.column, 21);

    const quint64 previousSequence = renderFrame.sequence;
    queue->append(QByteArrayLiteral("\x1b[2A\x1b[3G"));
    worker.drainOutput();
    queue->append(QByteArrayLiteral("\x1b[?25l"));
    worker.drainOutput();
    QTRY_VERIFY_WITH_TIMEOUT(store->latest().sequence > previousSequence, 2000);
    const TerminalFrameSnapshot live = store->latest();
    QCOMPARE(live.transportEpoch, renderFrame.transportEpoch);
    QCOMPARE(live.frame.cursor.row, 1);
    QCOMPARE(live.frame.cursor.column, 2);
    QVERIFY(!live.frame.cursor.visible);
    QCOMPARE(frameText(live.frame), frameText(renderFrame.frame));
    // Only the cursor moved: every row keeps its identity and revision.
    QCOMPARE(live.frame.lines.size(), renderFrame.frame.lines.size());
    for (qsizetype index = 0; index < live.frame.lines.size(); ++index) {
        QVERIFY(live.frame.lines.at(index).id == renderFrame.frame.lines.at(index).id);
        QCOMPARE(live.frame.lines.at(index).revision, renderFrame.frame.lines.at(index).revision);
    }

    const quint64 previousEpoch = live.transportEpoch;
    worker.beginTransportEpoch();
    const TerminalFrameSnapshot replacement = store->latest();
    QVERIFY(replacement.transportEpoch > previousEpoch);
    QCOMPARE(replacement.frame.cursor.row, 0);
    QCOMPARE(replacement.frame.cursor.column, 0);
    worker.shutdown();
}

void TerminalCoreWorkerTest::parsesSynchronizedUpdatesAcrossEverySplit() {
    const QByteArray stream = QByteArrayLiteral("before\x1b[?2026hinside\x1b[?2026lafter");
    QCOMPARE(parsePieces({stream}), (ParsedStream{stream, 1, 1}));
    QCOMPARE(firstDivergentSplit(stream), qsizetype(-1));

    const QByteArray repeated = QByteArrayLiteral("\x1b[?2026hA\x1b[?2026l\x1b[?2026hB\x1b[?2026l");
    QCOMPARE(parseBytewise(repeated), (ParsedStream{repeated, 2, 2}));

    // DEC private modes can be combined in one CSI. 2026 must still be
    // recognized regardless of its position in the parameter list.
    const QByteArray combined = QByteArrayLiteral(
        "x\x1b[?25;2026hcombined\x1b[?2026;25ly");
    QCOMPARE(parsePieces({combined}), (ParsedStream{combined, 1, 1}));
    QCOMPARE(firstDivergentSplit(combined), qsizetype(-1));

    const QByteArray malformed = QByteArrayLiteral("before\x1b[?") + QByteArray(300, '2') + QByteArrayLiteral("!after");
    QCOMPARE(parsePieces({malformed}).bytes, malformed);
}

void TerminalCoreWorkerTest::parsesRandomStreamsAlikeAcrossRandomSplits()
{
    QCOMPARE(span::tests::firstCounterexample(0x2026, 1000, randomSplitStream, parsesLikeWhole, describeSplit),
             QString());
}

void TerminalCoreWorkerTest::passesRandomStreamsThroughVerbatim()
{
    QCOMPARE(span::tests::firstCounterexample(0x1b5b, 1000, randomSplitStream, keepsEveryByte, describeSplit),
             QString());
}

void TerminalCoreWorkerTest::publishesIntermediateFramesAndObservesSynchronizedUpdates() {
    auto queue = std::make_shared<TerminalOutputQueue>();
    auto store = std::make_shared<TerminalFrameStore>();
    QVERIFY(queue->open() || queue->size() == 0);

    QThread thread;
    auto *worker = new TerminalCoreWorker(
        QStringLiteral("worker-test-%1").arg(QCoreApplication::applicationPid()),
        40, 6, 128, queue, store);
    worker->moveToThread(&thread);
    thread.start();
    attachQueueWake(queue, worker);
    const auto workerGuard = qScopeGuard([&]() {
        queue->clearWakeCallback();
        stopWorker(&thread, worker);
    });
    QVERIFY(QMetaObject::invokeMethod(worker, &TerminalCoreWorker::initialize,
                                      Qt::BlockingQueuedConnection));

    QMetaObject::invokeMethod(worker, [worker]() {
        worker->setRenderConsumer(true, 60);
    }, Qt::QueuedConnection);
    QTRY_VERIFY_WITH_TIMEOUT(store->latest().sequence > 0, 2000);

    const auto acknowledge = [&]() { acknowledgeLatest(*store); };
    acknowledge();

    // Feeds `bytes` and waits for a newer frame showing `visibleText`; the
    // caller acknowledges it or leaves it outstanding.
    const auto feed = [&](const QByteArray &bytes, const QString &visibleText) {
        const quint64 previous = store->latest().sequence;
        queue->append(bytes);
        QMetaObject::invokeMethod(worker, &TerminalCoreWorker::drainOutput,
                                  Qt::QueuedConnection);
        QTRY_VERIFY_WITH_TIMEOUT(store->latest().sequence > previous, 2000);
        QVERIFY2(frameText(store->latest().frame).contains(visibleText),
                 qPrintable(QStringLiteral("latest frame did not contain %1").arg(visibleText)));
    };

    const quint64 beforeSync = store->latest().sequence;
    queue->append(QByteArrayLiteral("\x1b[?2026h\x1b[2J\x1b[Hpartial_frame"));
    QMetaObject::invokeMethod(worker, &TerminalCoreWorker::drainOutput, Qt::QueuedConnection);
    QTest::qWait(80);
    QCOMPARE(store->latest().sequence, beforeSync);

    feed(QByteArrayLiteral("\x1b[2J\x1b[Hworker_frame_0\x1b[?2026l"),
         QStringLiteral("worker_frame_0"));
    acknowledge();
    QVERIFY(!store->latest().synchronizedUpdateActive);

    feed(QByteArrayLiteral("\x1b[?2026h\x1b[2J\x1b[Hworker_frame_1\x1b[?2026l"), QStringLiteral("worker_frame_1"));

    // One frame unacknowledged, then more than the queue capacity: no byte
    // lost, newer state coalesced into latest(), one notification held.
    const quint64 inFlight = store->latest().sequence;
    QVERIFY(store->notificationPending());
    const QByteArray sustained = repeatedPast(QByteArrayLiteral("\x1b[Hload"), kOutputQueueMaximumBytes + 64 * 1024)
        + QByteArrayLiteral("\x1b[2J\x1b[Hnewest_B");
    std::atomic_bool producerDone{false};
    std::thread producer([&]() {
        queue->append(sustained);
        producerDone.store(true);
    });
    const auto producerGuard = qScopeGuard([&]() { closeAndJoin(*queue, producer); });
    QTRY_VERIFY_WITH_TIMEOUT(producerDone.load(), 30000);
    QTRY_COMPARE_WITH_TIMEOUT(queue->size(), qsizetype(0), 30000);
    QVERIFY(store->notificationPending());
    // Coalesced frames may have advanced latest past the original notify seq.
    QVERIFY(store->latest().sequence >= inFlight);
    acknowledge();
    QTRY_VERIFY_WITH_TIMEOUT(
        frameText(store->latest().frame).contains(QStringLiteral("newest_B")), 10000);
    acknowledge();

    queue->close();
}

void TerminalCoreWorkerTest::suppressesHiddenFramesAndCapsPublicationRate() {
    auto queue = std::make_shared<TerminalOutputQueue>();
    auto store = std::make_shared<TerminalFrameStore>();
    queue->open();

    QThread thread;
    auto *worker = new TerminalCoreWorker(
        QStringLiteral("worker-rate-test-%1").arg(QCoreApplication::applicationPid()),
        80, 8, 128, queue, store);
    worker->moveToThread(&thread);
    thread.start();
    attachQueueWake(queue, worker);
    const auto workerGuard = qScopeGuard([&]() {
        queue->clearWakeCallback();
        stopWorker(&thread, worker);
    });
    QVERIFY(QMetaObject::invokeMethod(worker, &TerminalCoreWorker::initialize,
                                      Qt::BlockingQueuedConnection));

    QMetaObject::invokeMethod(worker, [worker]() {
        worker->setRenderConsumer(false, 60);
    }, Qt::BlockingQueuedConnection);
    QTest::qWait(80);

    // Parsing continues while hidden, and latest() still materializes so a
    // later session reattach can paint a complete grid immediately.
    queue->append(numberedRewrites(QByteArrayLiteral("hidden-"), 200));
    QMetaObject::invokeMethod(worker, &TerminalCoreWorker::drainOutput, Qt::QueuedConnection);
    QTRY_COMPARE_WITH_TIMEOUT(queue->size(), qsizetype(0), 2000);
    QTRY_VERIFY_WITH_TIMEOUT(
        frameText(store->latest().frame).contains(QStringLiteral("hidden-199")), 2000);
    const quint64 backgroundSequence = store->latest().sequence;
    QVERIFY(backgroundSequence > 0);
    QVERIFY(!store->notificationPending());

    QMetaObject::invokeMethod(worker, [worker]() {
        worker->setRenderConsumer(true, 30);
    }, Qt::QueuedConnection);
    QTRY_VERIFY_WITH_TIMEOUT(store->latest().sequence > backgroundSequence, 2000);
    QVERIFY(frameText(store->latest().frame).contains(QStringLiteral("hidden-199")));

    const auto acknowledge = [&]() { acknowledgeLatest(*store); };
    acknowledge();

    // Keep the renderer caught up under a much faster producer. At 30 Hz the
    // worker may publish the initial state plus about 8 frames in 250 ms; an
    // uncapped worker would publish one per update.
    const int published = pumpLineRewrites(*queue, worker, *store, 250);
    QVERIFY2(published <= 12,
             qPrintable(QStringLiteral("30 Hz worker published %1 frames in 250 ms")
                            .arg(published)));

    acknowledge();
    QMetaObject::invokeMethod(worker, [worker]() {
        worker->setRenderConsumer(false, 30);
    }, Qt::BlockingQueuedConnection);
    const quint64 hiddenSequence = store->latest().sequence;
    queue->append(QByteArrayLiteral("\rhidden-final\x1b[K"));
    QMetaObject::invokeMethod(worker, &TerminalCoreWorker::drainOutput, Qt::QueuedConnection);
    QTRY_COMPARE_WITH_TIMEOUT(queue->size(), qsizetype(0), 2000);
    QTRY_VERIFY_WITH_TIMEOUT(
        frameText(store->latest().frame).contains(QStringLiteral("hidden-final")), 2000);
    QVERIFY(store->latest().sequence > hiddenSequence);

    const quint64 beforeReattach = store->latest().sequence;
    QMetaObject::invokeMethod(worker, [worker]() {
        worker->setRenderConsumer(true, 120);
    }, Qt::QueuedConnection);
    QTRY_VERIFY_WITH_TIMEOUT(store->latest().sequence > beforeReattach, 2000);
    QVERIFY(frameText(store->latest().frame).contains(QStringLiteral("hidden-final")));
    acknowledge();

    // Reapplying the same consumer state or changing only its rate must not
    // manufacture an identical full-refresh snapshot.
    const quint64 configuredSequence = store->latest().sequence;
    QMetaObject::invokeMethod(worker, [worker]() {
        worker->setRenderConsumer(true, 120);
        worker->setRenderConsumer(true, 60);
    }, Qt::BlockingQueuedConnection);
    QTest::qWait(80);
    QCOMPARE(store->latest().sequence, configuredSequence);

    queue->close();
}

void TerminalCoreWorkerTest::coalescesTruecolorShimmerWhileNotificationPending() {
    // Mirrors Grok's welcome logo: same braille glyph, new truecolor each
    // synchronized frame, while the GUI has not yet ACKed the previous paint.
    auto queue = std::make_shared<TerminalOutputQueue>();
    auto store = std::make_shared<TerminalFrameStore>();
    QVERIFY(queue->open() || queue->size() == 0);

    TerminalCoreWorker worker(QStringLiteral("shimmer-coalesce"), 20, 4, 64, queue, store);
    worker.initialize();
    worker.setRenderConsumer(true, 60);
    QTRY_VERIFY_WITH_TIMEOUT(store->latest().sequence > 0, 2000);
    const quint64 baseline = store->latest().sequence;
    QVERIFY(store->acknowledgeNotification(baseline) ==
            FrameNotificationDisposition::Released);

    const QByteArray braille = QString::fromUtf8(u8"⣿").toUtf8();
    const auto feedShimmerFrame = [&](int red, int green, int blue) {
        const quint64 previous = store->latest().sequence;
        const QByteArray body =
            QByteArrayLiteral("\x1b[?2026h\x1b[H\x1b[38;2;") +
            QByteArray::number(red) + ';' + QByteArray::number(green) + ';' +
            QByteArray::number(blue) + 'm' + braille +
            QByteArrayLiteral("\x1b[?2026l");
        queue->append(body);
        worker.drainOutput();
        QTRY_VERIFY_WITH_TIMEOUT(store->latest().sequence > previous, 2000);
        const Frame frame = store->latest().frame;
        QVERIFY(!frame.lines.isEmpty());
        QVERIFY(!frame.lines.first().cells.isEmpty());
        const Cell &cell = frame.lines.first().cells.first();
        QCOMPARE(cell.text, QString::fromUtf8(u8"⣿"));
        QCOMPARE(cell.style.foreground, QColor(red, green, blue));
        QVERIFY(store->notificationPending());
    };

    feedShimmerFrame(108, 108, 108); // resting gray
    const quint64 firstPaint = store->latest().sequence;
    // Do not ACK — simulate a busy GUI holding the first frame notification.
    feedShimmerFrame(147, 147, 147); // mid-sweep
    feedShimmerFrame(180, 180, 180); // brighter
    QCOMPARE(store->latest().sequence > firstPaint, true);

    // ACK the sequence the GUI originally held. The reservation stays armed
    // and tells TerminalSession to issue exactly one follow-up notification.
    QVERIFY(store->acknowledgeNotification(firstPaint) ==
            FrameNotificationDisposition::NotifyAgain);
    QVERIFY(store->notificationPending());
    QCOMPARE(store->latest().frame.lines.first().cells.first().style.foreground,
             QColor(180, 180, 180));
    const quint64 brightFrame = store->latest().sequence;
    QVERIFY(store->acknowledgeNotification(brightFrame) ==
            FrameNotificationDisposition::Released);
    worker.shutdown();
}

void TerminalCoreWorkerTest::collapsesFragmentedRatatuiDrawTrajectory() {
    auto queue = std::make_shared<TerminalOutputQueue>();
    QVERIFY(queue->open() || queue->size() == 0);
    auto store = std::make_shared<TerminalFrameStore>();
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    TerminalCoreWorker worker(QStringLiteral("ratatui-cursor-sync"), 80, 12, 100,
                              queue, store);
    worker.initialize();
    worker.setRenderConsumer(true, 60);
    QTRY_VERIFY_WITH_TIMEOUT(store->latest().sequence > 0, 2000);
    const quint64 baseline = store->latest().sequence;

    // Crossterm emits MoveTo before each changed run, while Codex wraps the
    // complete Terminal::draw (including the final input cursor) in one
    // synchronized update. ConPTY may split that transaction anywhere; no
    // parser-pen visit is publishable before the matching end marker.
    queue->append(QByteArrayLiteral("\x1b[?2026h\x1b[2;2HWorking"));
    worker.drainOutput();
    QTest::qWait(25);
    QCOMPARE(store->latest().sequence, baseline);
    queue->append(QByteArrayLiteral(
        "\x1b[6;4HImprove documentation"));
    worker.drainOutput();
    QTest::qWait(25);
    QCOMPARE(store->latest().sequence, baseline);
    queue->append(QByteArrayLiteral("\x1b[10;3Hgpt-5.6-sol"));
    worker.drainOutput();
    QTest::qWait(25);
    QCOMPARE(store->latest().sequence, baseline);
    queue->append(QByteArrayLiteral("\x1b[6;4H\x1b[?2026l"));
    worker.drainOutput();

    QTRY_VERIFY_WITH_TIMEOUT(store->latest().sequence > baseline, 1000);
    const TerminalFrameSnapshot atomicFrame = store->latest();
    QCOMPARE(atomicFrame.frame.cursor.row, 5);
    QCOMPARE(atomicFrame.frame.cursor.column, 3);
    QVERIFY(atomicFrame.frame.cursor.visible);
    QVERIFY(frameText(atomicFrame.frame).contains(QStringLiteral("Working")));
    QVERIFY(frameText(atomicFrame.frame).contains(QStringLiteral("Improve documentation")));
    QVERIFY(frameText(atomicFrame.frame).contains(QStringLiteral("gpt-5.6-sol")));
    worker.shutdown();
}

void TerminalCoreWorkerTest::publishesSynchronizedCursorWithItsExactFrame() {
    auto queue = std::make_shared<TerminalOutputQueue>();
    QVERIFY(queue->open() || queue->size() == 0);
    auto store = std::make_shared<TerminalFrameStore>();
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    TerminalCoreWorker worker(QStringLiteral("cursor-frame-affinity"), 60, 12, 100,
                              queue, store);
    worker.initialize();
    worker.setRenderConsumer(true, 60);
    QTRY_VERIFY_WITH_TIMEOUT(store->latest().sequence > 0, 2000);
    const quint64 baseline = store->latest().sequence;

    queue->append(QByteArrayLiteral(
        "\x1b[?2026h\x1b[2J\x1b[2;2HStarting"
        "\x1b[6;4HExplain this codebase\x1b[6;4H\x1b[?2026l"));
    worker.drainOutput();
    QTRY_VERIFY_WITH_TIMEOUT(store->latest().sequence > baseline, 1000);
    TerminalFrameSnapshot first = store->latest();
    QCOMPARE(first.frame.cursor.row, 5);
    QCOMPARE(first.frame.cursor.column, 3);

    // A later frame moves the inline composer down. The frame store must not
    // expose the newer cells with the previous frame's cursor coordinate.
    queue->append(QByteArrayLiteral(
        "\x1b[?2026h\x1b[2J\x1b[3;2HStarting MCP servers"
        "\x1b[9;4HExplain this codebase\x1b[9;4H\x1b[?2026l"));
    worker.drainOutput();
    QTRY_VERIFY_WITH_TIMEOUT(store->latest().sequence > first.sequence, 1000);
    const TerminalFrameSnapshot second = store->latest();
    QCOMPARE(second.frame.cursor.row, 8);
    QCOMPARE(second.frame.cursor.column, 3);
    worker.shutdown();
}

void TerminalCoreWorkerTest::suppressesTransientHideShowDuringSynchronizedRedraw() {
    auto queue = std::make_shared<TerminalOutputQueue>();
    QVERIFY(queue->open() || queue->size() == 0);
    auto store = std::make_shared<TerminalFrameStore>();
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    TerminalCoreWorker worker(QStringLiteral("ratatui-hide-show"), 80, 12, 100,
                              queue, store);
    worker.initialize();
    worker.setRenderConsumer(true, 60);

    queue->append(QByteArrayLiteral(
        "\x1b[?2026h\x1b[?25h\x1b[6;4H\x1b[?2026l"));
    worker.drainOutput();
    QTRY_VERIFY_WITH_TIMEOUT(store->latest().frame.cursor.row == 5, 1000);
    QVERIFY(store->latest().frame.cursor.visible);
    QCOMPARE(store->latest().frame.cursor.column, 3);

    QCOMPARE(redrawsRestoringCursor(worker, *queue, *store, 3), 3);
    worker.shutdown();
}

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
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
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
    QTest::qWait(80);
    QCOMPARE(store->latest().sequence, baseline);

    // That bytes still arriving keep the transaction open is
    // TerminalPresentationSchedulerTest::releasesStalledSynchronizedUpdateOnlyWhenBytesStop.
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

void TerminalCoreWorkerTest::acceptsIdempotentSynchronizedUpdateModeChanges()
{
    auto queue = std::make_shared<TerminalOutputQueue>();
    auto store = std::make_shared<TerminalFrameStore>();
    QVERIFY(queue->open() || queue->size() == 0);

    TerminalCoreWorker worker(QStringLiteral("idempotent-sync-mode"), 20, 4, 64,
                              queue, store);
    worker.initialize();
    worker.setRenderConsumer(true, 60);
    QTest::failOnWarning("PTY sent nested synchronized-update begin");
    QTest::failOnWarning("PTY sent synchronized-update end without a begin");

    queue->append(QByteArrayLiteral(
        "\x1b[?2026l\x1b[?2026h\x1b[?2026h\x1b[Hmode-idempotent"
        "\x1b[?2026l\x1b[?2026l"));
    worker.drainOutput();

    QTRY_VERIFY_WITH_TIMEOUT(
        frameText(store->latest().frame).contains(QStringLiteral("mode-idempotent")),
        2000);
    QVERIFY(!store->latest().synchronizedUpdateActive);
    worker.shutdown();
}

void TerminalCoreWorkerTest::publishesUnsynchronizedRedrawAtSelectedFrameRate() {
    auto queue = std::make_shared<TerminalOutputQueue>();
    auto store = std::make_shared<TerminalFrameStore>();
    QVERIFY(queue->open() || queue->size() == 0);

    // An alternate-screen TUI without CSI ?2026 is not an atomic protocol
    // transaction. A normal drawing sweep must publish on the selected cap,
    // even though its terminal pen traverses many rows.
    TerminalCoreWorker worker(QStringLiteral("unsync-pen-frame-rate"), 40, 12, 64,
                              queue, store);
    worker.initialize();
    worker.setRenderConsumer(true, 360);
    QTRY_VERIFY_WITH_TIMEOUT(store->latest().sequence > 0, 2000);
    const quint64 baseline = store->latest().sequence;
    QVERIFY(store->acknowledgeNotification(baseline) ==
            FrameNotificationDisposition::Released);

    queue->append(QByteArrayLiteral("\x1b[?1049h\x1b[2J\x1b[?25h") + addressedRows(10));
    QElapsedTimer published;
    published.start();
    worker.drainOutput();

    // The removed quiet-edge heuristic imposed a 48ms floor here. At a
    // selected 360 Hz this complete, unsynchronized redraw must already have
    // reached the store well before that artificial floor.
    pumpUntil([&]() { return store->latest().sequence != baseline; }, published, 1000);
    QVERIFY2(store->latest().sequence > baseline && published.elapsed() < 48,
             qPrintable(QStringLiteral("unsynchronized redraw was held for %1ms")
                            .arg(published.elapsed())));
    const TerminalFrameSnapshot drawn = store->latest();
    QCOMPARE(drawn.frame.cursor.row, 9);
    QCOMPARE(drawn.frame.cursor.column, 7);
    QVERIFY(frameText(drawn.frame).contains(QStringLiteral("line-10")));

    // A later legitimate multi-row cursor jump must remain responsive too.
    const quint64 beforeShow = drawn.sequence;
    QVERIFY(store->acknowledgeNotification(beforeShow) ==
            FrameNotificationDisposition::Released);
    queue->append(QByteArrayLiteral("\x1b[?25l\x1b[10;1Hx\x1b[?25h\x1b[6;4H"));
    worker.drainOutput();
    QTRY_VERIFY_WITH_TIMEOUT(store->latest().sequence > beforeShow, 1000);
    const TerminalFrameSnapshot afterShow = store->latest();
    QCOMPARE(afterShow.frame.cursor.row, 5);
    QCOMPARE(afterShow.frame.cursor.column, 3);
    worker.shutdown();
}

void TerminalCoreWorkerTest::keepsSingleCellTypingResponsive() {
    auto queue = std::make_shared<TerminalOutputQueue>();
    auto store = std::make_shared<TerminalFrameStore>();
    QVERIFY(queue->open() || queue->size() == 0);

    TerminalCoreWorker worker(QStringLiteral("typing-frame-rate"), 40, 8, 64,
                              queue, store);
    worker.initialize();
    worker.setRenderConsumer(true, 60);
    QTRY_VERIFY_WITH_TIMEOUT(store->latest().sequence > 0, 2000);
    QVERIFY(store->acknowledgeNotification(store->latest().sequence) ==
            FrameNotificationDisposition::Released);

    queue->append(QByteArrayLiteral("\x1b[2;2Ha"));
    worker.drainOutput();
    QTRY_COMPARE_WITH_TIMEOUT(store->latest().frame.cursor.column, 2, 500);
    const quint64 afterA = store->latest().sequence;
    QVERIFY(store->acknowledgeNotification(afterA) ==
            FrameNotificationDisposition::Released);

    // Adjacent caret steps publish on the normal frame-rate schedule.
    queue->append(QByteArrayLiteral("b"));
    worker.drainOutput();
    QTRY_COMPARE_WITH_TIMEOUT(store->latest().frame.cursor.column, 3, 200);
    QVERIFY(store->latest().sequence > afterA);
    worker.shutdown();
}

void TerminalCoreWorkerTest::publishesContinuousUnsynchronizedMultiRowAnimation() {
    // Spinners and progress bars often redraw multiple rows without CSI ?2026.
    // They must continue publishing under the configured frame budget.
    auto queue = std::make_shared<TerminalOutputQueue>();
    auto store = std::make_shared<TerminalFrameStore>();
    QVERIFY(queue->open() || queue->size() == 0);

    TerminalCoreWorker worker(QStringLiteral("unsync-animation-stream"), 40, 8, 64,
                              queue, store);
    worker.initialize();
    worker.setRenderConsumer(true, 60);
    QTRY_VERIFY_WITH_TIMEOUT(store->latest().sequence > 0, 2000);
    QVERIFY(store->acknowledgeNotification(store->latest().sequence) ==
            FrameNotificationDisposition::Released);

    const int published = pumpTwoRowAnimation(*queue, worker, *store, 250);
    QCOMPARE(queue->size(), qsizetype(0));

    QVERIFY2(published >= 5,
             qPrintable(QStringLiteral(
                            "continuous unsync multi-row animation published only %1 frames")
                            .arg(published)));
    QVERIFY2(published <= 25,
             qPrintable(QStringLiteral(
                            "continuous unsync multi-row animation over-published %1 frames")
                            .arg(published)));
    QVERIFY(frameText(store->latest().frame).contains(QStringLiteral("row-a-")));
    QVERIFY(frameText(store->latest().frame).contains(QStringLiteral("row-b-")));
    worker.shutdown();
}

void TerminalCoreWorkerTest::holdsPartialProducerWriteUntilItsEnd() {
    auto queue = std::make_shared<TerminalOutputQueue>();
    auto store = std::make_shared<TerminalFrameStore>();
    QVERIFY(queue->open() || queue->size() == 0);
    TerminalCoreWorker worker(QStringLiteral("partial-write"), 40, 6, 64, queue, store);
    worker.initialize();
    worker.setRenderConsumer(true, 60);
    QTRY_VERIFY_WITH_TIMEOUT(store->latest().sequence > 0, 2000);
    const quint64 baseline = store->latest().sequence;
    store->acknowledgeNotification(baseline);
    QTest::qWait(30);

    // One unsynchronized TUI frame arrives in two PTY reads. The upper rows
    // alone are a torn frame and must not reach the store.
    queue->append(QByteArrayLiteral("\x1b[H\x1b[2Jtop-new\r\n"), false);
    worker.drainOutput();
    QTest::qWait(20);
    QCOMPARE(store->latest().sequence, baseline);
    queue->append(QByteArrayLiteral("bottom-new"));
    worker.drainOutput();
    QTRY_VERIFY_WITH_TIMEOUT(store->latest().sequence > baseline, 500);
    const TerminalFrameSnapshot complete = store->latest();
    QCOMPARE(complete.sequence, baseline + 1);
    QVERIFY(frameText(complete.frame).contains(QStringLiteral("top-new")));
    QVERIFY(frameText(complete.frame).contains(QStringLiteral("bottom-new")));
    store->acknowledgeNotification(complete.sequence);

    // A producer whose write never ends is still presented after the bounded
    // mid-write hold.
    queue->append(QByteArrayLiteral("unending"), false);
    worker.drainOutput();
    QTRY_VERIFY_WITH_TIMEOUT(store->latest().sequence > complete.sequence, 500);
    QVERIFY(frameText(store->latest().frame).contains(QStringLiteral("unending")));
    worker.shutdown();
}

QTEST_MAIN(TerminalCoreWorkerTest)

#include "terminal_core_worker_test.moc"
