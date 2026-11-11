#include "terminal/api/terminal_session.h"
#include "terminal/api/terminal_process.h"
#include "terminal/render/terminal_ime_anchor_resolver.h"
#include "terminal/render/terminal_view.h"
#include "terminal/core/ghostty_engine.h"
#include <QAbstractEventDispatcher>
#include <QScopeGuard>
#include <stdexcept>
#include <QByteArrayList>

#include <QClipboard>
#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QGuiApplication>
#include <QInputMethodEvent>
#include <QKeyEvent>
#include <QList>
#include <QMouseEvent>
#include <QQuickWindow>
#include <QSemaphore>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTextCharFormat>
#include <QTest>
#include <QThread>
#include <QTimer>
#include <QWheelEvent>

#include <memory>
#include <atomic>
#include <mutex>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <imm.h>

using namespace span::terminal;

namespace {

// Windows routes IME hotkeys (Ctrl+Space toggles the Chinese IME) and
// composition only to windows that have an input context associated. Tests
// run on the offscreen platform, which has no native IME; there the check
// reads what the windows platform bases that association on: the focus
// object's Qt::ImEnabled answer.
bool windowHasImeContext(QWindow &window)
{
    if (QGuiApplication::platformName() != QLatin1String("windows")) {
        QObject *focus = window.focusObject();
        if (!focus) {
            return false;
        }
        QInputMethodQueryEvent query(Qt::ImEnabled);
        QCoreApplication::sendEvent(focus, &query);
        return query.value(Qt::ImEnabled).toBool();
    }
    const HWND hwnd = reinterpret_cast<HWND>(window.winId());
    const HIMC context = ImmGetContext(hwnd);
    if (!context) {
        return false;
    }
    ImmReleaseContext(hwnd, context);
    return true;
}

class FakeTerminalProcess final : public TerminalProcess {
public:
    explicit FakeTerminalProcess(std::shared_ptr<QSemaphore> startGate = {})
        : m_startGate(std::move(startGate)) {}

    bool start(const StartOptions &options,
               OutputCallback outputCallback,
               ExitCallback exitCallback,
               QString *) override
    {
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_output = std::move(outputCallback);
            m_exit = std::move(exitCallback);
            m_startColumns = options.columns;
            m_startRows = options.rows;
        }
        if (m_startGate) {
            m_startGate->acquire();
        }
        m_running.store(true, std::memory_order_release);
        return true;
    }

    bool write(const QByteArray &bytes, TerminalInputKind = TerminalInputKind::User) override
    {
        if (!isRunning()) {
            return false;
        }
        std::lock_guard<std::mutex> lock(m_mutex);
        m_writes.append(bytes);
        return true;
    }
    bool cancelPendingWritesAndInterrupt() override {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_writes.append(QByteArray(1, char(0x03)));
        return isRunning();
    }
    void requestResize(int columns, int rows) override
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_resizes.append(QSize(columns, rows));
    }
    // Every size the Core told the child, oldest first.
    QList<QSize> resizes() const
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_resizes;
    }
    QSize lastResize() const
    {
        const QList<QSize> sizes = resizes();
        return sizes.isEmpty() ? QSize() : sizes.last();
    }
    QSize startingSize() const {
        std::lock_guard<std::mutex> lock(m_mutex);
        return QSize(m_startColumns, m_startRows);
    }
    QList<QByteArray> writtenBytes() const
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_writes;
    }
    void emitOutput(const QByteArray &bytes)
    {
        OutputCallback output;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            output = m_output;
        }
        if (output) output(bytes, true);
    }
    void clearScreen() override { m_clears.fetch_add(1, std::memory_order_acq_rel); }
    int clearCount() const { return m_clears.load(std::memory_order_acquire); }
    void stop() override { m_running.store(false, std::memory_order_release); }
    bool isRunning() const override {
        return m_running.load(std::memory_order_acquire);
    }
    qsizetype pendingWriteBytes() const override { return 0; }
    quint32 processId() const override { return isRunning() ? 42 : 0; }
    QList<quint32> jobProcessIds() const override { return {}; }

private:
    mutable std::mutex m_mutex;
    OutputCallback m_output;
    ExitCallback m_exit;
    std::shared_ptr<QSemaphore> m_startGate;
    std::atomic_bool m_running{false};
    std::atomic_int m_clears{0};
    int m_startColumns = 0;
    int m_startRows = 0;
    QList<QSize> m_resizes;
    QList<QByteArray> m_writes;
};

class FrameConsumer final : public QObject {
public:
    explicit FrameConsumer(TerminalSession &session) {
        const quint64 token = session.attachRenderConsumer(this);
        connect(&session, &TerminalSession::frameReady, this, [&session, token](quint64 sequence) {
            session.acknowledgeFrame(token, sequence);
        });
        session.setRenderConsumer(token, true, 60);
    }
};

QString quoteCommand(const QString &path)
{
    return QStringLiteral("\"%1\"").arg(QDir::toNativeSeparators(path));
}

QString frameText(const Frame &frame)
{
    QString result;
    for (const Line &line : frame.lines) {
        for (const Cell &cell : line.cells) {
            if (!cell.continuation) {
                result.append(cell.text.isEmpty() ? QStringLiteral(" ") : cell.text);
            }
        }
        result.append(QLatin1Char('\n'));
    }
    return result;
}

// The Core has published a frame on the session's latest viewport.
bool gridSettled(const TerminalSession &session)
{
    const Frame frame = session.latestSnapshot().frame;
    return frame.columns == session.columns() && frame.rows == session.rows();
}

// Output can be visible before the queued started notification, and input
// before running is dropped: wait for both.
bool waitForChildOutput(const TerminalSession &session, const QString &marker, int timeout)
{
    return QTest::qWaitFor([&] {
        return session.running()
            && frameText(session.latestSnapshot().frame).contains(marker);
    }, timeout);
}

bool frameContainsHyperlink(const Frame &frame, const QString &url)
{
    for (const Line &line : frame.lines) {
        for (const Cell &cell : line.cells) {
            if (cell.style.hyperlink == url) {
                return true;
            }
        }
    }
    return false;
}

bool viewContainsHyperlink(const TerminalView &view, const QString &url)
{
    for (int row = 0; row < 12; ++row) {
        for (int column = 0; column < 80; ++column) {
            if (view.linkAtCell(row, column) == url) {
                return true;
            }
        }
    }
    return false;
}

// Cells of `frame` painted with a background of their own.
int coloredBackgroundCells(const Frame &frame)
{
    int count = 0;
    for (const Line &line : frame.lines) {
        for (const Cell &cell : line.cells) count += !cell.continuation && !cell.style.defaultBackground;
    }
    return count;
}
QByteArray kittyGreenGraphic()
{
    QByteArray rgb(8 * 16 * 3, '\0');
    for (int index = 0; index < 8 * 16; ++index) {
        rgb[index * 3 + 1] = char(255);
    }
    return QByteArrayLiteral("\x1b_Ga=T,f=24,s=8,v=16,c=2,r=2,i=1;")
        + rgb.toBase64() + QByteArrayLiteral("\x1b\\");
}


QVariantMap childOptions(const QString &arguments = {})
{
    const QString childPath = QDir(QCoreApplication::applicationDirPath())
                                  .filePath(QStringLiteral("pty_echo_child.exe"));
    const QString commandLine = arguments.isEmpty()
        ? quoteCommand(childPath)
        : QStringLiteral("%1 %2").arg(quoteCommand(childPath), arguments);
    return {
        {QStringLiteral("commandLine"), commandLine},
        {QStringLiteral("columns"), 80},
        {QStringLiteral("rows"), 12},
    };
}

// An engine factory whose first build throws; `attempts` counts every call.
TerminalSession::EngineFactory failingFirstEngine(std::shared_ptr<std::atomic_int> attempts)
{
    return [attempts](int columns, int rows) -> std::unique_ptr<GhosttyEngine> {
        if (attempts->fetch_add(1) == 0) throw std::runtime_error("core unavailable");
        return std::make_unique<GhosttyEngine>(columns, rows);
    };
}

// Requests a `rows`-high viewport and replays the child's full repaint at that
// height once the size reached it. Empty when the prompt and cursor stayed
// together, otherwise what went wrong.
QString repaintAfterResize(TerminalSession &session, FakeTerminalProcess &process, int rows, quint64 epoch)
{
    session.requestViewport(210, rows, epoch);
    if (!QTest::qWaitFor([&]() { return process.lastResize() == QSize(210, rows); }, 1000))
        return QStringLiteral("resize to %1 rows never reached the process").arg(rows);
    const QByteArray prompt = "size" + QByteArray::number(rows) + "> ";
    process.emitOutput("\x1b[?25l\x1b[H" + prompt + "\x1b[K" + QByteArray("\r\n\x1b[K").repeated(rows - 1)
                       + "\x1b[1;9H\x1b[?25h");
    const auto promptOnTop = [&]() {
        const Frame frame = session.latestSnapshot().frame;
        return frameText(frame).startsWith(QString::fromLatin1(prompt)) && frame.cursor.row == 0
            && frame.cursor.column == 8;
    };
    if (!QTest::qWaitFor(promptOnTop, 1000))
        return QStringLiteral("%1 rows: prompt and cursor apart after the repaint").arg(rows);
    const TerminalScrollbar scrollbar = session.latestSnapshot().frame.scrollbar;
    if (scrollbar.total > scrollbar.rows) return QStringLiteral("%1 rows: the repaint went into history").arg(rows);
    if (!process.writtenBytes().isEmpty()) return QStringLiteral("%1 rows: wrote to the child").arg(rows);
    return {};
}

// Column and row of the first occurrence of `needle`; (-1, -1) if none.
QPoint findText(const QStringList &rows, const QString &needle)
{
    for (int row = 0; row < rows.size(); ++row) {
        const int column = rows.at(row).indexOf(needle);
        if (column >= 0) return QPoint(column, row);
    }
    return QPoint(-1, -1);
}

QVariantList huePalette()
{
    QVariantList palette;
    for (int index = 0; index < 16; ++index) palette.append(QColor::fromHsv(index * 22, 200, 230));
    return palette;
}

QByteArray numberedLines(const QByteArray &prefix, int count)
{
    QByteArray lines;
    for (int line = 0; line < count; ++line) lines += prefix + QByteArray::number(line) + "\r\n";
    return lines;
}

} // namespace

class TerminalSessionTest final : public QObject {
    Q_OBJECT

private slots:
    void preservesSelectedFrameRate();
    void hiddenViewDoesNotResizeSharedSession();
    void visibleViewStartsNeverLaunchedSession();
    void searchCompletesWithoutMatches();
    void hiddenViewDoesNotStartNeverLaunchedSession();
    void startsCoreThreadLazily();
    void plainCtrlCInterruptsForegroundOnFirstPress();
    void plainCtrlCCopiesSelectionWithoutInterrupting();
    void forwardsKeyReleaseOnlyAfterItsPress();
    void attachesImeWhileFocusedViewCanType();
    void clearsViewWhenSessionIsDestroyed();
    void switchesViewWithoutReusingPreviousSessionFrame();
    void restartReplacesTheBoundViewsGrid();
    void graphicsDisappearAcrossSessionBinding();
    void viewRendersDecorationsSearchAndMouse();
    void roundTripsThroughConPty();
    void restartsWithFreshProtocolEpoch();
    void keepsPreeditLocalAcrossPartialCommit();
    void wheelScrollFinishesImeComposition();
    void resolvesHiddenCursorFromRealConPtyFrame();
    void capturesFragmentedSynchronizedCursorInFinalFrame();
    void viewportResizeConvergesOnLatestAndNeverBlocksGui();
    void viewportResizeDoesNotInjectTerminalInput();
    void resizeRepaintKeepsPromptAndCursorTogether();
    void viewportRequestedDuringStartConvergesAfterStart();
    void stopDoesNotWaitForBlockedProcessStart();
    void destructionDoesNotWaitForBlockedProcessStart();
    void retriesThreeTimesAndAllowsExplicitReconnect();
    void stopRevokesPendingRetry();
    void coreFailureRebuildsBeforeStartingBackend();
    void engineRebuildFailureFailsStart();
    void interruptRevokesQueuedAndReturnedInput();
    void consumerTransferRevokesQueuedRequests();
    void slowPreparationConvergesOnLatestViewport();
};

void TerminalSessionTest::retriesThreeTimesAndAllowsExplicitReconnect()
{
    auto attempts = std::make_shared<std::atomic_int>(0);
    auto times = std::make_shared<QList<qint64>>();
    auto mutex = std::make_shared<std::mutex>();
    QElapsedTimer clock;
    clock.start();
    TerminalSession session(QStringLiteral("retry"), QDir::tempPath(), {}, nullptr,
        [attempts, times, mutex, &clock]() -> std::unique_ptr<TerminalProcess> {
            const std::lock_guard lock(*mutex);
            times->append(clock.elapsed());
            attempts->fetch_add(1);
            return {};
        });
    session.setRetryIntervalForTesting(100);
    QVERIFY(session.start());
    QVERIFY(session.starting());
    QVERIFY(session.start()); // Repeated calls while starting do not reset the budget.
    QTRY_VERIFY_WITH_TIMEOUT(session.failed(), 3000);
    QCOMPARE(attempts->load(), 3);
    QVERIFY(!session.starting());
    QVERIFY(!session.running());
    QVERIFY(session.startupError().contains(QStringLiteral("factory")));
    {
        const std::lock_guard lock(*mutex);
        QVERIFY(times->at(1) - times->at(0) >= 90);
        QVERIFY(times->at(2) - times->at(1) >= 90);
    }
    QTest::qWait(200);
    QCOMPARE(attempts->load(), 3);
    QVERIFY(session.start());
    QTRY_COMPARE_WITH_TIMEOUT(attempts->load(), 6, 3000);
    QTRY_VERIFY_WITH_TIMEOUT(session.failed(), 1000);
}

void TerminalSessionTest::stopRevokesPendingRetry()
{
    auto attempts = std::make_shared<std::atomic_int>(0);
    TerminalSession session(QStringLiteral("cancel-retry"), QDir::tempPath(), {}, nullptr,
        [attempts]() -> std::unique_ptr<TerminalProcess> {
            attempts->fetch_add(1);
            return {};
        });
    session.setRetryIntervalForTesting(100);
    QVERIFY(session.start());
    QTRY_VERIFY_WITH_TIMEOUT(!session.startupError().isEmpty(), 1000);
    QVERIFY(session.starting());
    session.stop();
    QVERIFY(!session.starting());
    QTest::qWait(200);
    QCOMPARE(attempts->load(), 1);
    QVERIFY(!session.running());
}

void TerminalSessionTest::coreFailureRebuildsBeforeStartingBackend()
{
    auto coreAttempts = std::make_shared<std::atomic_int>(0);
    auto processAttempts = std::make_shared<std::atomic_int>(0);
    TerminalSession session(
        QStringLiteral("retry-core"), QDir::tempPath(), {}, nullptr,
        [processAttempts]() -> std::unique_ptr<TerminalProcess> {
            processAttempts->fetch_add(1);
            return std::make_unique<FakeTerminalProcess>();
        },
        failingFirstEngine(coreAttempts));
    QVERIFY(session.start());
    QTRY_VERIFY_WITH_TIMEOUT(!session.startupError().isEmpty(), 1000);
    QCOMPARE(processAttempts->load(), 0);
    QVERIFY(!session.running());
    QTRY_VERIFY_WITH_TIMEOUT(session.running(), 2000);
    QCOMPARE(coreAttempts->load(), 2);
    QCOMPARE(processAttempts->load(), 1);
    QCOMPARE(session.coreState(), TerminalSession::CoreReady);
    QVERIFY(session.startupError().isEmpty());
}

void TerminalSessionTest::engineRebuildFailureFailsStart()
{
    // The first engine starts, but no engine for a later transport can be
    // built: a failed epoch must report an error instead of publishing a
    // dead "running" session.
    auto built = std::make_shared<std::atomic_int>(0);
    TerminalSession session(
        QStringLiteral("epoch-rebuild"), QDir::tempPath(), {}, nullptr,
        []() -> std::unique_ptr<TerminalProcess> { return std::make_unique<FakeTerminalProcess>(); },
        [built](int columns, int rows) -> std::unique_ptr<GhosttyEngine> {
            if (built->fetch_add(1) > 0) throw std::runtime_error("no engine for this transport");
            return std::make_unique<GhosttyEngine>(columns, rows);
        });
    QVERIFY(session.start());
    QTRY_VERIFY_WITH_TIMEOUT(session.running(), 2000);

    session.stop();
    QVERIFY(session.start());
    // The new transport's engine fails: the session must surface an error
    // instead of appearing to run with the old child's engine.
    QTRY_VERIFY_WITH_TIMEOUT(!session.startupError().isEmpty(), 3000);
    QVERIFY(!session.running());
}

void TerminalSessionTest::interruptRevokesQueuedAndReturnedInput()
{
    std::atomic<FakeTerminalProcess *> process{nullptr};
    TerminalSession session(QStringLiteral("cancel-input"), QDir::tempPath(), {}, nullptr,
        [&process]() -> std::unique_ptr<TerminalProcess> {
            auto child = std::make_unique<FakeTerminalProcess>();
            process.store(child.get());
            return child;
        });
    QVERIFY(session.start());
    QTRY_VERIFY_WITH_TIMEOUT(session.running(), 1000);
    auto *dispatcher = session.findChild<QThread *>()->eventDispatcher();
    QVERIFY(dispatcher);
    QSemaphore entered, release;
    const auto unblock = qScopeGuard([&]() { release.release(); });
    QMetaObject::invokeMethod(dispatcher, [&]() {
        entered.release();
        release.acquire();
    }, Qt::QueuedConnection);
    QVERIFY(entered.tryAcquire(1, 1000));
    session.sendText(QStringLiteral("queued-old"));
    session.paste(QStringLiteral("paste-old"));
    process.load()->emitOutput(QByteArrayLiteral("\x1b[5n"));
    session.interrupt();
    release.release();
    QTRY_VERIFY_WITH_TIMEOUT(process.load()->writtenBytes().join("").contains("\x1b[0n"), 1000);
    QByteArray delivered = process.load()->writtenBytes().join("");
    QVERIFY(delivered.startsWith(QByteArray(1, char(0x03))));
    QVERIFY(!delivered.contains("old"));

    session.sendText(QStringLiteral("returned-old"));
    // Core has encoded the input; its queued GUI delivery has not run yet.
    QMetaObject::invokeMethod(dispatcher, []() {}, Qt::BlockingQueuedConnection);
    session.interrupt();
    session.sendText(QStringLiteral("new"));
    QTRY_VERIFY_WITH_TIMEOUT(process.load()->writtenBytes().join("").endsWith("new"), 1000);
    delivered = process.load()->writtenBytes().join("");
    QVERIFY(!delivered.contains("old"));
    QCOMPARE(delivered.count(char(0x03)), 2);
}

void TerminalSessionTest::consumerTransferRevokesQueuedRequests()
{
    std::atomic<FakeTerminalProcess *> process{nullptr};
    TerminalSession session(QStringLiteral("consumer-transfer"), QDir::tempPath(),
        {{QStringLiteral("rows"), 3}}, nullptr,
        [&process]() -> std::unique_ptr<TerminalProcess> {
            auto child = std::make_unique<FakeTerminalProcess>();
            process.store(child.get());
            return child;
        });
    auto first = std::make_unique<QObject>();
    QObject second;
    const quint64 oldToken = session.attachRenderConsumer(first.get());
    session.setRenderConsumer(oldToken, true, 60);
    QVERIFY(session.start());
    QTRY_VERIFY_WITH_TIMEOUT(session.running(), 1000);
    process.load()->emitOutput(QByteArrayLiteral("one\r\ntwo\r\nthree\r\nfour\r\nfive\r\nsix"));
    QTRY_VERIFY_WITH_TIMEOUT(session.scrollbackLines() >= 3, 1000);
    auto *dispatcher = session.findChild<QThread *>()->eventDispatcher();
    QSemaphore entered, release;
    const auto unblock = qScopeGuard([&]() { release.release(); });
    QMetaObject::invokeMethod(dispatcher, [&]() { entered.release(); release.acquire(); },
                              Qt::QueuedConnection);
    QVERIFY(entered.tryAcquire(1, 1000));
    // Queued while the first consumer still owns the session.
    session.setRenderConsumer(oldToken, true, 1);
    const quint64 token = session.attachRenderConsumer(&second);
    session.setRenderConsumer(token, true, 60);
    first.reset();
    session.detachRenderConsumer(oldToken);
    session.setRenderConsumer(oldToken, false, 1);
    QVERIFY(session.ownsRenderConsumer(token));
    release.release();
    QMetaObject::invokeMethod(dispatcher, []() {}, Qt::BlockingQueuedConnection);
    // The revoked consumer neither pauses nor throttles the new one.
    QSignalSpy ready(&session, &TerminalSession::frameReady);
    session.acknowledgeFrame(oldToken, session.latestSnapshot().sequence);
    session.acknowledgeFrame(token, session.latestSnapshot().sequence);
    process.load()->emitOutput(QByteArrayLiteral("\r\nseven"));
    QTRY_VERIFY_WITH_TIMEOUT(frameText(session.latestSnapshot().frame).contains(QStringLiteral("seven")), 1000);
    QTRY_VERIFY_WITH_TIMEOUT(!ready.isEmpty(), 1000);
    session.detachRenderConsumer(token);
    QVERIFY(!session.ownsRenderConsumer(token));
}

void TerminalSessionTest::slowPreparationConvergesOnLatestViewport()
{
    std::atomic<FakeTerminalProcess *> process{nullptr};
    TerminalSession session(QStringLiteral("prepare-slow"), QDir::tempPath(), {}, nullptr,
        [&process]() -> std::unique_ptr<TerminalProcess> {
            auto child = std::make_unique<FakeTerminalProcess>();
            process.store(child.get());
            return child;
        });
    QVERIFY(session.start());
    QTRY_VERIFY_WITH_TIMEOUT(session.running(), 1000);
    auto *dispatcher = session.findChild<QThread *>()->eventDispatcher();
    QSemaphore entered, release;
    const auto unblock = qScopeGuard([&]() { release.release(); });
    QMetaObject::invokeMethod(dispatcher, [&]() { entered.release(); release.acquire(); },
                              Qt::QueuedConnection);
    QVERIFY(entered.tryAcquire(1, 1000));
    QSignalSpy errors(&session, &TerminalSession::errorOccurred);
    session.commitViewport(150, 50);
    session.commitViewport(160, 55);
    QCOMPARE(session.columns(), 160);
    // The backend never learns a size before Core has resized its grid.
    QTest::qWait(300);
    QVERIFY(process.load()->resizes().isEmpty());
    release.release();
    QTRY_COMPARE_WITH_TIMEOUT(process.load()->lastResize(), QSize(160, 55), 1000);
    QTRY_COMPARE_WITH_TIMEOUT(session.latestSnapshot().frame.columns, 160, 1000);
    QCOMPARE(errors.count(), 0);
}

void TerminalSessionTest::viewportResizeConvergesOnLatestAndNeverBlocksGui()
{
    std::atomic<FakeTerminalProcess *> fake{nullptr};
    TerminalSession session(
        QStringLiteral("fake-viewport"), QDir::tempPath(),
        {{QStringLiteral("columns"), 80}, {QStringLiteral("rows"), 24}},
        nullptr,
        [&fake]() -> std::unique_ptr<TerminalProcess> {
            auto process = std::make_unique<FakeTerminalProcess>();
            fake.store(process.get(), std::memory_order_release);
            return process;
        });
    QVERIFY(session.start());
    QTRY_VERIFY_WITH_TIMEOUT(fake.load(std::memory_order_acquire) != nullptr,
                             1000);
    QTRY_VERIFY_WITH_TIMEOUT(session.running(), 1000);
    FakeTerminalProcess *process = fake.load(std::memory_order_acquire);

    bool guiHeartbeat = false;
    QTimer::singleShot(0, &session, [&guiHeartbeat]() {
        guiHeartbeat = true;
    });
    session.requestViewport(100, 30, 1);
    session.requestViewport(120, 36, 2);
    session.requestViewport(140, 42, 3);
    QCOMPARE(session.columns(), 140);
    QCOMPARE(session.rows(), 42);
    QTRY_VERIFY_WITH_TIMEOUT(guiHeartbeat, 250);
    QTRY_COMPARE_WITH_TIMEOUT(process->lastResize(), QSize(140, 42), 1000);
    QTRY_COMPARE_WITH_TIMEOUT(
        session.latestSnapshotShared()->viewportEpoch, quint64(3), 1000);

    // An older generation is ignored.
    const qsizetype told = process->resizes().size();
    session.requestViewport(90, 25, 2);
    QCOMPARE(session.columns(), 140);
    QTest::qWait(25);
    QCOMPARE(process->resizes().size(), told);

    // A new generation of the same grid republishes without resizing the child.
    session.requestViewport(140, 42, 4);
    QTRY_COMPARE_WITH_TIMEOUT(session.latestSnapshotShared()->viewportEpoch, quint64(4), 1000);
    QCOMPARE(process->resizes().size(), told);
}

void TerminalSessionTest::viewportResizeDoesNotInjectTerminalInput()
{
    std::atomic<FakeTerminalProcess *> fake{nullptr};
    TerminalSession session(
        QStringLiteral("fake-resize-input"), QDir::tempPath(),
        {{QStringLiteral("columns"), 80}, {QStringLiteral("rows"), 24}},
        nullptr,
        [&fake]() -> std::unique_ptr<TerminalProcess> {
            auto process = std::make_unique<FakeTerminalProcess>();
            fake.store(process.get(), std::memory_order_release);
            return process;
        });
    QVERIFY(session.start());
    QTRY_VERIFY_WITH_TIMEOUT(fake.load(std::memory_order_acquire) != nullptr,
                             1000);
    QTRY_VERIFY_WITH_TIMEOUT(session.running(), 1000);
    FakeTerminalProcess *process = fake.load(std::memory_order_acquire);

    session.requestViewport(120, 36, 1);
    QTRY_COMPARE_WITH_TIMEOUT(process->lastResize(), QSize(120, 36), 1000);
    QTest::qWait(50);
    QVERIFY(process->writtenBytes().isEmpty());
}

void TerminalSessionTest::resizeRepaintKeepsPromptAndCursorTogether()
{
    std::atomic<FakeTerminalProcess *> fake{nullptr};
    TerminalSession session(
        QStringLiteral("resize-repaint"), QDir::tempPath(),
        {{QStringLiteral("columns"), 120}, {QStringLiteral("rows"), 32}},
        nullptr, [&fake]() -> std::unique_ptr<TerminalProcess> {
            auto process = std::make_unique<FakeTerminalProcess>();
            fake.store(process.get(), std::memory_order_release);
            return process;
        });
    QVERIFY(session.start());
    QTRY_VERIFY_WITH_TIMEOUT(session.running(), 1000);
    auto *process = fake.load(std::memory_order_acquire);
    QVERIFY(process);
    FrameConsumer consumer(session);
    process->emitOutput(QByteArrayLiteral("prompt> "));
    QTRY_VERIFY_WITH_TIMEOUT(frameText(session.latestSnapshot().frame).startsWith(
                                QStringLiteral("prompt> ")), 1000);

    // Replays the observed repaint shape: grow, shrink, grow again.
    QCOMPARE(repaintAfterResize(session, *process, 63, 1), QString());
    QCOMPARE(repaintAfterResize(session, *process, 29, 2), QString());
    QCOMPARE(repaintAfterResize(session, *process, 63, 3), QString());
}

void TerminalSessionTest::viewportRequestedDuringStartConvergesAfterStart()
{
    const auto startGate = std::make_shared<QSemaphore>();
    std::atomic<FakeTerminalProcess *> fake{nullptr};
    TerminalSession session(
        QStringLiteral("fake-start-viewport"), QDir::tempPath(),
        {{QStringLiteral("columns"), 80}, {QStringLiteral("rows"), 24}},
        nullptr,
        [&fake, startGate]() -> std::unique_ptr<TerminalProcess> {
            auto process = std::make_unique<FakeTerminalProcess>(startGate);
            fake.store(process.get(), std::memory_order_release);
            return process;
        });

    QVERIFY(session.start());
    QTRY_VERIFY_WITH_TIMEOUT(fake.load(std::memory_order_acquire) != nullptr,
                             1000);
    FakeTerminalProcess *process = fake.load(std::memory_order_acquire);
    session.requestViewport(132, 41, 9);
    QCOMPARE(session.columns(), 132);
    startGate->release();

    QTRY_VERIFY_WITH_TIMEOUT(session.running(), 1000);
    QCOMPARE(process->startingSize(), QSize(80, 24));
    // The child started at the old size; the Core resizes it once attached.
    QTRY_COMPARE_WITH_TIMEOUT(process->lastResize(), QSize(132, 41), 1000);
    QTRY_COMPARE_WITH_TIMEOUT(
        session.latestSnapshotShared()->viewportEpoch, quint64(9), 1000);
}

void TerminalSessionTest::destructionDoesNotWaitForBlockedProcessStart()
{
    const auto startGate = std::make_shared<QSemaphore>();
    std::atomic<FakeTerminalProcess *> fake{nullptr};
    auto *session = new TerminalSession(
        QStringLiteral("fake-blocked-start"), QDir::tempPath(), {}, nullptr,
        [&fake, startGate]() -> std::unique_ptr<TerminalProcess> {
            auto process = std::make_unique<FakeTerminalProcess>(startGate);
            fake.store(process.get(), std::memory_order_release);
            return process;
        });
    QVERIFY(session->start());
    QTRY_VERIFY_WITH_TIMEOUT(fake.load(std::memory_order_acquire) != nullptr,
                             1000);

    QElapsedTimer destruction;
    destruction.start();
    delete session;
    QVERIFY2(destruction.elapsed() < 100, "TerminalSession destruction waited for process start");
    startGate->release();
}

void TerminalSessionTest::stopDoesNotWaitForBlockedProcessStart()
{
    const auto startGate = std::make_shared<QSemaphore>();
    std::atomic<FakeTerminalProcess *> fake{nullptr};
    TerminalSession session(
        QStringLiteral("fake-blocked-stop"), QDir::tempPath(), {}, nullptr,
        [&fake, startGate]() -> std::unique_ptr<TerminalProcess> {
            auto process = std::make_unique<FakeTerminalProcess>(startGate);
            fake.store(process.get(), std::memory_order_release);
            return process;
        });
    QVERIFY(session.start());
    QTRY_VERIFY_WITH_TIMEOUT(fake.load(std::memory_order_acquire) != nullptr,
                             1000);

    QElapsedTimer stopLatency;
    stopLatency.start();
    session.stop();
    QVERIFY2(stopLatency.elapsed() < 100, "TerminalSession::stop waited for process start");
    QVERIFY(!session.running());

    startGate->release();
    QTest::qWait(50);
    QVERIFY(!session.running());
}

void TerminalSessionTest::preservesSelectedFrameRate()
{
    TerminalView view;
    view.setMaximumFrameRate(30);
    QCOMPARE(view.maximumFrameRate(), 30);
    view.setMaximumFrameRate(144);
    QCOMPARE(view.maximumFrameRate(), 144);
    view.setMaximumFrameRate(360);
    QCOMPARE(view.maximumFrameRate(), 360);
    view.setMaximumFrameRate(1);
    QCOMPARE(view.maximumFrameRate(), 30);
    view.setMaximumFrameRate(999);
    QCOMPARE(view.maximumFrameRate(), 360);
}

void TerminalSessionTest::hiddenViewDoesNotResizeSharedSession()
{
    QQuickWindow window;
    window.resize(1000, 400);
    TerminalView visibleView;
    visibleView.setParentItem(window.contentItem());
    visibleView.setSize(QSizeF(800, 400));
    visibleView.setVisible(true);
    TerminalView hiddenView;
    hiddenView.setParentItem(window.contentItem());
    hiddenView.setSize(QSizeF(160, 400));
    hiddenView.setVisible(false);
    window.show();
    QTRY_VERIFY_WITH_TIMEOUT(window.isExposed(), 5000);

    TerminalSession session(QStringLiteral("shared-resize"), QDir::tempPath());
    session.start();
    visibleView.setSession(&session);
    hiddenView.setSession(&session);
    visibleView.polish();
    QCoreApplication::processEvents();
    QTRY_VERIFY_WITH_TIMEOUT(session.columns() > 40, 5000);
    const int wideColumns = session.columns();
    const int wideRows = session.rows();

    hiddenView.setSize(QSizeF(120, 240));
    hiddenView.polish();
    QCoreApplication::processEvents();
    QCOMPARE(session.columns(), wideColumns);
    QCOMPARE(session.rows(), wideRows);
}

void TerminalSessionTest::visibleViewStartsNeverLaunchedSession()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    QQuickWindow window;
    window.resize(800, 240);
    TerminalView view;
    view.setParentItem(window.contentItem());
    view.setSize(QSizeF(window.width(), window.height()));
    window.show();
    QTRY_VERIFY_WITH_TIMEOUT(window.isExposed(), 5000);

    TerminalSession session(QStringLiteral("canvas-start"), directory.path(),
                            childOptions());
    QVERIFY(!session.launchAttempted());
    view.setSession(&session);
    view.polish();
    QTRY_VERIFY_WITH_TIMEOUT(session.running(), 8000);
    QVERIFY(session.launchAttempted());
}

void TerminalSessionTest::searchCompletesWithoutMatches()
{
    QQuickWindow window;
    window.resize(800, 240);
    TerminalView view;
    view.setParentItem(window.contentItem());
    view.setSize(QSizeF(window.width(), window.height()));
    window.show();
    QTRY_VERIFY_WITH_TIMEOUT(window.isExposed(), 5000);

    TerminalSession session(QStringLiteral("search-stop"), QDir::tempPath(), {}, nullptr,
        []() -> std::unique_ptr<TerminalProcess> {
            return std::make_unique<FakeTerminalProcess>();
        });
    view.setSession(&session);
    view.polish();
    QTRY_VERIFY_WITH_TIMEOUT(session.running(), 5000);

    // The count is pending until a frame reports it for this needle; a
    // needle with no matches completes at zero.
    view.setSearch(QStringLiteral("anything"));
    QVERIFY(view.searchPending());
    QTRY_VERIFY_WITH_TIMEOUT(!view.searchPending(), 3000);
    QVERIFY(view.searchActive());
    QCOMPARE(view.searchMatchCount(), 0);
    QCOMPARE(view.searchCurrentIndex(), 0);

    // Stopping keeps the last answer instead of reopening the count.
    session.stop();
    QVERIFY(!view.searchPending());
}

void TerminalSessionTest::hiddenViewDoesNotStartNeverLaunchedSession()
{
    QQuickWindow window;
    window.resize(800, 240);
    TerminalView view;
    view.setParentItem(window.contentItem());
    view.setSize(QSizeF(window.width(), window.height()));
    view.setVisible(false);
    window.show();
    QTRY_VERIFY_WITH_TIMEOUT(window.isExposed(), 5000);

    TerminalSession session(QStringLiteral("hidden-start"), QDir::tempPath());
    view.setSession(&session);
    view.polish();
    QCoreApplication::processEvents();
    QVERIFY(!session.launchAttempted());
    QVERIFY(!session.running());
}

void TerminalSessionTest::startsCoreThreadLazily()
{
    TerminalSession session(QStringLiteral("lazy-core"), QDir::tempPath());
    QThread *coreThread = session.findChild<QThread *>();
    QVERIFY(coreThread);
    QVERIFY(!coreThread->isRunning());

    const quint64 token = session.attachRenderConsumer(this);
    session.setRenderConsumer(token, true, 60);
    QTRY_VERIFY_WITH_TIMEOUT(coreThread->isRunning(), 2000);
    QTRY_VERIFY_WITH_TIMEOUT(session.latestSnapshot().sequence > 0, 2000);
}

void TerminalSessionTest::plainCtrlCInterruptsForegroundOnFirstPress()
{
    const QString childPath = QDir(QCoreApplication::applicationDirPath())
                                  .filePath(QStringLiteral("pty_echo_child.exe"));
    QVERIFY2(QFileInfo::exists(childPath), qPrintable(childPath));
    QTemporaryDir directory;
    QVERIFY(directory.isValid());

    TerminalSession session(QStringLiteral("single-ctrl-c"), directory.path(),
                            childOptions(QStringLiteral("raw-tui")));
    TerminalView view;
    view.setSession(&session);
    QVERIFY(session.start());
    QVERIFY(waitForChildOutput(session, QStringLiteral("span_raw_tui_ready"), 5000));

    QKeyEvent press(QEvent::KeyPress, Qt::Key_C, Qt::ControlModifier);
    QVERIFY(QCoreApplication::sendEvent(&view, &press));
    QVERIFY(press.isAccepted());
    QTRY_VERIFY_WITH_TIMEOUT(!session.running(), 5000);
}

void TerminalSessionTest::plainCtrlCCopiesSelectionWithoutInterrupting()
{
    const QString childPath = QDir(QCoreApplication::applicationDirPath())
                                  .filePath(QStringLiteral("pty_echo_child.exe"));
    QVERIFY2(QFileInfo::exists(childPath), qPrintable(childPath));
    QTemporaryDir directory;
    QVERIFY(directory.isValid());

    TerminalSession session(QStringLiteral("selection-ctrl-c"), directory.path(),
                            childOptions());
    QVERIFY(session.start());
    QVERIFY(waitForChildOutput(session, QStringLiteral("span_child_ready"), 5000));

    // The selection lives in the core; the view learns of it from the frames
    // it presents, which takes a window.
    QQuickWindow window;
    window.resize(800, 400);
    TerminalView view;
    view.setParentItem(window.contentItem());
    view.setSize(QSizeF(window.width(), window.height()));
    window.show();
    if (!QTest::qWaitFor([&]() { return window.isExposed(); }, 5000))
        QSKIP("Window not exposed; selection presentation not verified.");
    view.setSession(&session);
    QTRY_VERIFY_WITH_TIMEOUT(gridSettled(session), 5000);
    QVERIFY(waitForChildOutput(session, QStringLiteral("span_child_ready"), 5000));

    const Frame readyFrame = session.latestSnapshot().frame;
    const QPoint ready = findText(frameText(readyFrame).split(QLatin1Char('\n')), QStringLiteral("span_child_ready"));
    QVERIFY(ready.y() >= readyFrame.viewportRow);
    // Frames carry overscan rows above the viewport.
    const int readyRow = ready.y() - readyFrame.viewportRow;
    const int readyColumn = ready.x();
    const QPointF start(view.cellWidth() * (readyColumn + 0.25),
                        view.cellHeight() * (readyRow + 0.5));
    const QPointF end(view.cellWidth() * (readyColumn + 4.25),
                      view.cellHeight() * (readyRow + 0.5));
    QMouseEvent mousePress(QEvent::MouseButtonPress, start, start, start,
                           Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
    QMouseEvent mouseMove(QEvent::MouseMove, end, end, end,
                          Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
    QMouseEvent mouseRelease(QEvent::MouseButtonRelease, end, end, end,
                             Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
    QVERIFY(QCoreApplication::sendEvent(&view, &mousePress));
    QVERIFY(QCoreApplication::sendEvent(&view, &mouseMove));
    QVERIFY(QCoreApplication::sendEvent(&view, &mouseRelease));
    QTRY_VERIFY_WITH_TIMEOUT(view.hasSelection(), 3000);
    QGuiApplication::clipboard()->clear();
    QKeyEvent copyPress(QEvent::KeyPress, Qt::Key_C, Qt::ControlModifier);
    QVERIFY(QCoreApplication::sendEvent(&view, &copyPress));
    QVERIFY(copyPress.isAccepted());
    QCOMPARE(QGuiApplication::clipboard()->text(), QStringLiteral("span"));
    QVERIFY(session.running());
    session.stop();
}

void TerminalSessionTest::forwardsKeyReleaseOnlyAfterItsPress()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    TerminalSession session(QStringLiteral("key-release-pairing"), directory.path(),
                            childOptions());
    TerminalView view;
    view.setSession(&session);
    QVERIFY(session.start());
    QVERIFY(waitForChildOutput(session, QStringLiteral("span_child_ready"), 5000));

    // The IME's Ctrl+Space toggle consumes the press and lets the release
    // through; the child must not read a key coming up that never went down.
    QKeyEvent orphan(QEvent::KeyRelease, Qt::Key_Space, Qt::ControlModifier,
                     QStringLiteral(" "));
    QCoreApplication::sendEvent(&view, &orphan);
    QVERIFY(!orphan.isAccepted());

    QKeyEvent press(QEvent::KeyPress, Qt::Key_X, Qt::NoModifier, QStringLiteral("x"));
    QKeyEvent release(QEvent::KeyRelease, Qt::Key_X, Qt::NoModifier, QStringLiteral("x"));
    QKeyEvent secondRelease(QEvent::KeyRelease, Qt::Key_X, Qt::NoModifier,
                            QStringLiteral("x"));
    QCoreApplication::sendEvent(&view, &press);
    QCoreApplication::sendEvent(&view, &release);
    QCoreApplication::sendEvent(&view, &secondRelease);
    QVERIFY(press.isAccepted());
    QVERIFY(release.isAccepted());
    QVERIFY(!secondRelease.isAccepted());
    session.stop();
}

void TerminalSessionTest::attachesImeWhileFocusedViewCanType()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    TerminalSession session(QStringLiteral("ime-enabled"), directory.path(),
                            childOptions());
    QQuickWindow window;
    window.resize(400, 200);
    TerminalView view;
    view.setParentItem(window.contentItem());
    view.setSize(QSizeF(window.width(), window.height()));
    view.setSession(&session);
    window.show();
    if (!QTest::qWaitForWindowExposed(&window)) QSKIP("Window exposure unavailable; IME context not verified.");
    window.requestActivate();
    if (!QTest::qWaitFor([&]() { return window.isActive(); }, 5000))
        QSKIP("Window activation unavailable; IME context not verified.");
    view.forceActiveFocus();
    QTRY_VERIFY(view.hasActiveFocus());
    // Focused before the child runs: there is nothing to type into yet.
    QTRY_VERIFY(!windowHasImeContext(window));

    // Focus stays put while the session starts, so only the view can tell
    // Qt that input is possible now.
    QVERIFY(session.start());
    QVERIFY(waitForChildOutput(session, QStringLiteral("span_child_ready"), 5000));
    QTRY_VERIFY(windowHasImeContext(window));

    session.stop();
    QTRY_VERIFY_WITH_TIMEOUT(!session.running(), 5000);
    QTRY_VERIFY(!windowHasImeContext(window));
    view.setParentItem(nullptr);
}

void TerminalSessionTest::clearsViewWhenSessionIsDestroyed()
{
    auto session = std::make_unique<TerminalSession>(
        QStringLiteral("view-lifetime"), QDir::tempPath());
    TerminalView view;
    QSignalSpy changed(&view, &TerminalView::sessionChanged);
    view.setSession(session.get());
    QCOMPARE(view.session(), session.get());

    session.reset();
    QCOMPARE(view.session(), nullptr);
    QCOMPARE(changed.count(), 2);
}

void TerminalSessionTest::switchesViewWithoutReusingPreviousSessionFrame()
{
    const QString childPath = QDir(QCoreApplication::applicationDirPath())
                                  .filePath(QStringLiteral("pty_echo_child.exe"));
    QVERIFY2(QFileInfo::exists(childPath), qPrintable(childPath));
    QTemporaryDir directory;
    QVERIFY(directory.isValid());

    TerminalSession first(QStringLiteral("view-switch-first"), directory.path(),
                          childOptions());
    TerminalSession second(QStringLiteral("view-switch-second"), directory.path(),
                           childOptions());
    FrameConsumer firstConsumer(first);
    FrameConsumer secondConsumer(second);
    QVERIFY(first.start());
    QVERIFY(second.start());
    QVERIFY(waitForChildOutput(first, QStringLiteral("span_child_ready"), 8000));
    QVERIFY(waitForChildOutput(second, QStringLiteral("span_child_ready"), 8000));

    first.sendText(QStringLiteral("link-one"));
    first.sendKeyStroke(TerminalKeyStroke::named(TerminalKey::Enter));
    second.sendText(QStringLiteral("link-two"));
    second.sendKeyStroke(TerminalKeyStroke::named(TerminalKey::Enter));
    const QString firstUrl = QStringLiteral("https://span.test/link-one");
    const QString secondUrl = QStringLiteral("https://span.test/link-two");
    QTRY_VERIFY_WITH_TIMEOUT(
        frameContainsHyperlink(first.latestSnapshot().frame, firstUrl), 8000);
    QTRY_VERIFY_WITH_TIMEOUT(
        frameContainsHyperlink(second.latestSnapshot().frame, secondUrl), 8000);

    TerminalView view;
    view.setSize(QSizeF(800, 400));
    view.setSession(&first);
    view.polish();
    QCoreApplication::processEvents();
    QTRY_VERIFY_WITH_TIMEOUT(viewContainsHyperlink(view, firstUrl), 5000);
    QVERIFY(!viewContainsHyperlink(view, secondUrl));

    // Switch without producing any new PTY output. The already published frame
    // of the newly bound session must replace every view-local cache at once.
    view.setSession(&second);
    view.polish();
    QCoreApplication::processEvents();
    QTRY_VERIFY_WITH_TIMEOUT(viewContainsHyperlink(view, secondUrl), 5000);
    QVERIFY(!viewContainsHyperlink(view, firstUrl));

    // Bounce again before queued notifications from the previous attachment
    // have drained, then process them and prove they cannot overwrite A.
    view.setSession(&first);
    view.polish();
    QCoreApplication::processEvents();
    QTRY_VERIFY_WITH_TIMEOUT(viewContainsHyperlink(view, firstUrl), 5000);
    QVERIFY(!viewContainsHyperlink(view, secondUrl));
}

// Which row nodes a view keeps across sessions and restarts is pinned in
// terminal_scene_graph_test; here the session side: a restart replaces the
// grid the bound view presents instead of carrying the old cells over.
void TerminalSessionTest::restartReplacesTheBoundViewsGrid()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    TerminalSession session(
        QStringLiteral("scene-restart"), directory.path(),
        childOptions(QStringLiteral("scene-switch")));
    QSignalSpy epochChanged(&session, &TerminalSession::transportEpochChanged);
    QVERIFY(session.start());
    QTRY_VERIFY_WITH_TIMEOUT(session.running(), 8000);

    session.sendText(QStringLiteral("scene-first"));
    session.sendKeyStroke(TerminalKeyStroke::named(TerminalKey::Enter));
    QTRY_COMPARE_WITH_TIMEOUT(session.title(), QStringLiteral("span_scene_first"), 8000);

    QQuickWindow window;
    window.setColor(Qt::black);
    window.resize(800, 300);
    TerminalView view;
    view.setParentItem(window.contentItem());
    view.setSize(QSizeF(window.width(), window.height()));
    window.show();
    QTRY_VERIFY_WITH_TIMEOUT(window.isExposed(), 5000);

    view.setSession(&session);
    QTRY_COMPARE_WITH_TIMEOUT(coloredBackgroundCells(session.latestSnapshot().frame), 20, 5000);
    const quint64 firstTransportEpoch = session.latestSnapshot().transportEpoch;

    session.stop();
    QVERIFY(session.start());
    QCOMPARE(epochChanged.count(), 1);
    QTRY_VERIFY_WITH_TIMEOUT(session.running(), 8000);
    QTRY_VERIFY_WITH_TIMEOUT(
        session.latestSnapshot().transportEpoch > firstTransportEpoch, 8000);
    // The scene-switch child deliberately produces no startup cells. The
    // transport boundary itself must replace the previous grid.
    QTRY_COMPARE_WITH_TIMEOUT(coloredBackgroundCells(session.latestSnapshot().frame), 0, 5000);
    QCOMPARE(view.session(), &session);

    view.setParentItem(nullptr);
    session.stop();
}

void TerminalSessionTest::viewRendersDecorationsSearchAndMouse()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const auto fake = std::make_shared<std::atomic<FakeTerminalProcess *>>(nullptr);
    TerminalSession session(
        QStringLiteral("view-decorations"), directory.path(),
        {{QStringLiteral("columns"), 80}, {QStringLiteral("rows"), 24}},
        nullptr,
        [fake]() -> std::unique_ptr<TerminalProcess> {
            auto process = std::make_unique<FakeTerminalProcess>();
            fake->store(process.get(), std::memory_order_release);
            return process;
        });
    QVERIFY(session.start());
    QTRY_VERIFY_WITH_TIMEOUT(session.running(), 1000);
    FakeTerminalProcess *process = fake->load(std::memory_order_acquire);
    QVERIFY(process);

    QQuickWindow window;
    window.setColor(Qt::black);
    window.resize(800, 300);
    TerminalView view;
    view.setParentItem(window.contentItem());
    view.setSize(QSizeF(window.width(), window.height()));
    window.show();
    if (!QTest::qWaitFor([&]() { return window.isExposed(); }, 5000))
        QSKIP("Window not exposed; view rendering not verified.");
    view.setSession(&session);
    QCoreApplication::processEvents();
    QTRY_VERIFY_WITH_TIMEOUT(!process->resizes().isEmpty(), 5000);
    QTRY_VERIFY_WITH_TIMEOUT(gridSettled(session), 5000);

    // Every underline style, a tinted underline, strike, overline and blink,
    // then glyphs the renderer draws itself: box drawing, blocks, braille.
    process->emitOutput(
        "\x1b]133;A\x07$ \x1b]133;B\x07"
        "\x1b[4:1mone\x1b[4:2mtwo\x1b[4:3mcurly\x1b[4:4mdot\x1b[4:5mdash\x1b[0m "
        "\x1b[58:5:2m\x1b[4mtint\x1b[0m \x1b[9mstrike\x1b[0m \x1b[53mover\x1b[0m "
        "\x1b[5mblink\x1b[0m \x1b[42m  \x1b[0m\r\n"
        "\xe2\x94\x8c\xe2\x94\x80\xe2\x94\x90 \xe2\x95\xad\xe2\x95\xae "
        "\xe2\x96\x88\xe2\x96\x93\xe2\x96\x91 \xe2\xa0\xbf\xe2\xa3\xbf\r\n"
        "needle haystack needle\r\n");
    QTRY_VERIFY_WITH_TIMEOUT(frameText(session.latestSnapshot().frame)
                                 .contains(QStringLiteral("needle haystack")), 5000);

    // The view takes a new palette and foreground while rows are on screen;
    // how cells map onto palette colors is terminal_row_layout_test's.
    QSignalSpy recolored(&view, &TerminalView::colorsChanged);
    view.setAnsiPalette(huePalette());
    view.setForegroundColor(QColor(Qt::yellow));
    QCOMPARE(recolored.count(), 2);
    QCOMPARE(view.ansiPalette(), huePalette());
    QCOMPARE(view.foregroundColor(), QColor(Qt::yellow));

    // The child may also recolor: X11 rgb: specs of 1 to 4 hex digits per
    // channel, an invalid spec is ignored, and queries report 16-bit channels.
    process->emitOutput("]10;rgb:f/80/000]11;rgb:0000/0000/2020"
                        "]11;rgb:zz/00/00]10;?]11;?");
    QTRY_VERIFY_WITH_TIMEOUT(process->writtenBytes().join().contains(
                                 "]10;rgb:ffff/8080/0000")
                                 && process->writtenBytes().join().contains(
                                     "]11;rgb:0000/0000/2020"),
                             3000);

    // Literal text, ASCII case ignored. A new needle selects the newest
    // match; stepping back moves the selection to the older one.
    view.setSearch(QStringLiteral("NEEDLE"));
    QTRY_VERIFY_WITH_TIMEOUT(view.searchMatchCount() == 2 && !view.searchPending() && view.searchCurrentIndex() > 0,
                             5000);
    const int newest = view.searchCurrentIndex();
    view.findNext(true);
    QTRY_VERIFY_WITH_TIMEOUT(view.searchCurrentIndex() > 0 && view.searchCurrentIndex() != newest, 3000);
    view.clearSearch();
    QVERIFY(!view.searchActive());
    QCOMPARE(view.searchMatchCount(), 0);

    // No prompt marks: prompt jumps leave the viewport where it is.
    view.revealPrompt(-1);
    view.revealPrompt(1);
    QTest::qWait(100);
    QCOMPARE(view.scrollOffset(), 0.0);
    const QQuickItem &item = view;
    QVERIFY(item.inputMethodQuery(Qt::ImCursorRectangle).toRectF().isValid());

    // A styled preedit is drawn locally at the cursor and tells the IME where
    // its caret is; the surrounding text is the cursor's line.
    QInputMethodEvent preedit(
        QStringLiteral("pinyin"),
        {{QInputMethodEvent::TextFormat, 0, 6, QTextCharFormat()},
         {QInputMethodEvent::Cursor, 3, 1, {}}});
    QVERIFY(QCoreApplication::sendEvent(&view, &preedit));
    QVERIFY(item.inputMethodQuery(Qt::ImCursorRectangle).toRectF().isValid());
    QVERIFY(item.inputMethodQuery(Qt::ImAnchorRectangle).toRectF().isValid());
    const QString surrounding = item.inputMethodQuery(Qt::ImSurroundingText).toString();
    const int caret = item.inputMethodQuery(Qt::ImCursorPosition).toInt();
    QCOMPARE(item.inputMethodQuery(Qt::ImTextBeforeCursor).toString()
                 + item.inputMethodQuery(Qt::ImTextAfterCursor).toString(),
             surrounding);
    QCOMPARE(item.inputMethodQuery(Qt::ImTextBeforeCursor).toString().size(), caret);
    QInputMethodEvent commit(QString(), {});
    commit.setCommitString(QStringLiteral("ok"));
    QVERIFY(QCoreApplication::sendEvent(&view, &commit));
    QTRY_VERIFY_WITH_TIMEOUT(process->writtenBytes().join().contains("ok"), 3000);

    // A double click selects the word under the pointer.
    const QPointF word(view.cellWidth() * 9.5, view.cellHeight() * 2.5);
    // Qt delivers the second press before the double-click event; the core
    // counts presses by their timestamps.
    QMouseEvent press(QEvent::MouseButtonPress, word, word, word,
                      Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
    QMouseEvent release(QEvent::MouseButtonRelease, word, word, word,
                        Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
    QMouseEvent pressAgain(QEvent::MouseButtonPress, word, word, word, Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
    QMouseEvent twice(QEvent::MouseButtonDblClick, word, word, word,
                      Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
    press.setTimestamp(1000);
    release.setTimestamp(1050);
    pressAgain.setTimestamp(1100);
    twice.setTimestamp(1100);
    QCoreApplication::sendEvent(&view, &press);
    QCoreApplication::sendEvent(&view, &release);
    QCoreApplication::sendEvent(&view, &pressAgain);
    QCoreApplication::sendEvent(&view, &twice);
    QCoreApplication::sendEvent(&view, &release);
    QTRY_VERIFY_WITH_TIMEOUT(view.selectedText().contains(QStringLiteral("haystack")), 3000);
    QVERIFY(!view.selectedText().contains(QStringLiteral("needle")));

    // A click on an OSC 8 link opens it; right and middle clicks ask for the
    // menu and paste the clipboard.
    QSignalSpy linked(&view, &TerminalView::linkActivated);
    QSignalSpy menu(&view, &TerminalView::contextMenuRequested);
    process->emitOutput("\x1b[6;1H\x1b]8;;https://example.com/x\x07LINK\x1b]8;;\x07");
    QTRY_VERIFY_WITH_TIMEOUT(frameText(session.latestSnapshot().frame)
                                 .contains(QStringLiteral("LINK")), 3000);
    const auto click = [&](QPointF at, Qt::MouseButton button) {
        QMouseEvent down(QEvent::MouseButtonPress, at, at, at, button, button, Qt::NoModifier);
        QMouseEvent up(QEvent::MouseButtonRelease, at, at, at, button, Qt::NoButton,
                       Qt::NoModifier);
        QCoreApplication::sendEvent(&view, &down);
        QCoreApplication::sendEvent(&view, &up);
    };
    const QPointF link(view.cellWidth() * 1.5, view.cellHeight() * 5.5);
    QTRY_VERIFY_WITH_TIMEOUT(([&]() {
        click(link, Qt::LeftButton);
        return !linked.isEmpty();
    }()), 3000);
    QCOMPARE(linked.constFirst().constFirst().toString(), QStringLiteral("https://example.com/x"));
    click(link, Qt::RightButton);
    QCOMPARE(menu.size(), 1);
    QCOMPARE(menu.constFirst().at(2).toString(), QStringLiteral("https://example.com/x"));
    QGuiApplication::clipboard()->setText(QStringLiteral("clip-text"));
    click(word, Qt::MiddleButton);
    QTRY_VERIFY_WITH_TIMEOUT(process->writtenBytes().join().contains("clip-text"), 3000);

    // A bracketed paste larger than one slice arrives whole and framed.
    process->emitOutput("\x1b[?2004h\x1b[7;1Hmode");
    QTRY_VERIFY_WITH_TIMEOUT(frameText(session.latestSnapshot().frame)
                                 .contains(QStringLiteral("mode")), 3000);
    session.paste(QString(5000, QLatin1Char('p')) + QStringLiteral("END"));
    QTRY_VERIFY_WITH_TIMEOUT(process->writtenBytes().join().contains("pEND\x1b[201~"), 5000);
    QVERIFY(process->writtenBytes().join().contains("\x1b[200~"));

    // The child's queries are answered through the same input queue.
    process->emitOutput("\x1b[c");
    QTRY_VERIFY_WITH_TIMEOUT(process->writtenBytes().join().contains("\x1b[?"), 3000);

    // Bell, notifications and taskbar progress reach the session's owner.
    QSignalSpy bell(&session, &TerminalSession::bellRang);
    QSignalSpy notified(&session, &TerminalSession::notificationRequested);
    process->emitOutput("\x07\x1b]9;built\x07\x1b]777;notify;Title;Body\x07\x1b]9;4;1;40\x07");
    QTRY_COMPARE_WITH_TIMEOUT(bell.size(), 1, 3000);
    QTRY_COMPARE_WITH_TIMEOUT(notified.size(), 2, 3000);
    QCOMPARE(notified.at(1).at(0).toString(), QStringLiteral("Title"));
    QTRY_COMPARE_WITH_TIMEOUT(session.progressValue(), 40, 3000);

    // On the alternate screen the wheel becomes cursor keys. The view routes
    // the wheel by the frame it presents, which may lag the session's
    // snapshot, so the wheel is repeated until it takes effect.
    process->emitOutput("\x1b[?1049h");
    QTRY_VERIFY_WITH_TIMEOUT(session.latestSnapshot().frame.alternateScreen, 3000);
    const qsizetype beforeWheel = process->writtenBytes().join().size();
    QTRY_VERIFY_WITH_TIMEOUT(([&]() {
        QWheelEvent arrows(word, word, QPoint(), QPoint(0, 360), Qt::NoButton, Qt::NoModifier,
                           Qt::NoScrollPhase, false);
        QCoreApplication::sendEvent(&view, &arrows);
        return process->writtenBytes().join().mid(beforeWheel).contains("A");
    }()), 3000);
    process->emitOutput("\x1b[?1049l");

    // Scrolled up, new output is flagged unread until the view is back down.
    process->emitOutput(numberedLines("scroll line ", 60));
    QTRY_VERIFY_WITH_TIMEOUT(session.scrollbackLines() > 20, 5000);
    // The session already holds the main screen, but until the view presents
    // it the wheel still becomes cursor keys rather than scrolling, so the
    // wheel is repeated until the view scrolls.
    QTRY_VERIFY_WITH_TIMEOUT(([&]() {
        QWheelEvent up(word, word, QPoint(), QPoint(0, 120), Qt::NoButton, Qt::NoModifier,
                       Qt::NoScrollPhase, false);
        QCoreApplication::sendEvent(&view, &up);
        return view.scrollOffset() > 0;
    }()), 5000);
    process->emitOutput("fresh output\r\n");
    QTRY_VERIFY_WITH_TIMEOUT(view.unreadOutput(), 5000);
    view.scrollToBottom();
    QTRY_VERIFY_WITH_TIMEOUT(!view.unreadOutput(), 3000);
    // The backend's screen is cleared after ours.
    session.clearDisplay();
    QTRY_VERIFY_WITH_TIMEOUT(!frameText(session.latestSnapshot().frame)
                                 .contains(QStringLiteral("fresh output")), 3000);
    QTRY_COMPARE_WITH_TIMEOUT(process->clearCount(), 1, 3000);
    process->emitOutput("\x1b[?2004l");

    // Once the application asks for SGR mouse reports, clicks go to it.
    process->emitOutput("\x1b[?1000h\x1b[?1006h");
    const auto reported = [process]() {
        return process->writtenBytes().join().contains("\x1b[<0;");
    };
    QTRY_VERIFY_WITH_TIMEOUT(([&]() {
        QMouseEvent click(QEvent::MouseButtonPress, word, word, word,
                          Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
        QMouseEvent up(QEvent::MouseButtonRelease, word, word, word,
                       Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
        QCoreApplication::sendEvent(&view, &click);
        QCoreApplication::sendEvent(&view, &up);
        return reported();
    }()), 5000);

    // Wheel notches and sideways tilts become buttons 64/65 and 66/67.
    const auto wheel = [&](QPoint pixels, QPoint angle) {
        QWheelEvent event(word, word, pixels, angle, Qt::NoButton, Qt::NoModifier,
                          Qt::NoScrollPhase, false);
        QCoreApplication::sendEvent(&view, &event);
        return event.isAccepted();
    };
    QVERIFY(wheel({}, {0, 120}));
    QVERIFY(wheel({}, {0, -120}));
    QVERIFY(wheel({0, 1}, {}));
    QVERIFY(wheel({}, {120, 0}));
    QVERIFY(wheel({}, {-120, 0}));
    QTRY_VERIFY_WITH_TIMEOUT(process->writtenBytes().join().contains("\x1b[<64;")
                                 && process->writtenBytes().join().contains("\x1b[<65;")
                                 && process->writtenBytes().join().contains("\x1b[<66;")
                                 && process->writtenBytes().join().contains("\x1b[<67;"),
                             3000);

    view.setParentItem(nullptr);
    session.stop();
}

void TerminalSessionTest::graphicsDisappearAcrossSessionBinding()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());

    const auto firstFake = std::make_shared<std::atomic<FakeTerminalProcess *>>(nullptr);
    const auto secondFake = std::make_shared<std::atomic<FakeTerminalProcess *>>(nullptr);
    TerminalSession first(
        QStringLiteral("graphics-bind-first"), directory.path(),
        {{QStringLiteral("columns"), 80}, {QStringLiteral("rows"), 24}},
        nullptr,
        [firstFake]() -> std::unique_ptr<TerminalProcess> {
            auto process = std::make_unique<FakeTerminalProcess>();
            firstFake->store(process.get(), std::memory_order_release);
            return process;
        });
    TerminalSession second(
        QStringLiteral("graphics-bind-second"), directory.path(),
        {{QStringLiteral("columns"), 80}, {QStringLiteral("rows"), 24}},
        nullptr,
        [secondFake]() -> std::unique_ptr<TerminalProcess> {
            auto process = std::make_unique<FakeTerminalProcess>();
            secondFake->store(process.get(), std::memory_order_release);
            return process;
        });

    QVERIFY(first.start());
    QVERIFY(second.start());
    QTRY_VERIFY_WITH_TIMEOUT(first.running(), 1000);
    QTRY_VERIFY_WITH_TIMEOUT(second.running(), 1000);
    FakeTerminalProcess *firstProcess = firstFake->load(std::memory_order_acquire);
    FakeTerminalProcess *secondProcess = secondFake->load(std::memory_order_acquire);
    QVERIFY(firstProcess);
    QVERIFY(secondProcess);

    QQuickWindow window;
    window.setColor(Qt::black);
    window.resize(800, 300);
    TerminalView view;
    view.setParentItem(window.contentItem());
    view.setSize(QSizeF(window.width(), window.height()));
    window.show();
    if (!QTest::qWaitFor([&]() { return window.isExposed(); }, 5000))
        QSKIP("Window not exposed; graphics binding not verified.");

    view.setSession(&first);
    QCoreApplication::processEvents();
    QTRY_VERIFY_WITH_TIMEOUT(!firstProcess->resizes().isEmpty(), 5000);
    QTRY_VERIFY_WITH_TIMEOUT(gridSettled(first), 5000);

    firstProcess->emitOutput(kittyGreenGraphic());
    QTRY_COMPARE_WITH_TIMEOUT(first.latestSnapshot().frame.graphics.size(), 1, 5000);
    QTRY_VERIFY_WITH_TIMEOUT(first.latestSnapshot().frame.graphics.first().resource != nullptr,
                             5000);

    view.setSession(&second);
    QCoreApplication::processEvents();
    QTRY_VERIFY_WITH_TIMEOUT(!secondProcess->resizes().isEmpty(), 5000);
    QTRY_VERIFY_WITH_TIMEOUT(gridSettled(second), 5000);
    QTRY_VERIFY_WITH_TIMEOUT(second.latestSnapshot().sequence > 0, 5000);
    QTRY_COMPARE_WITH_TIMEOUT(second.latestSnapshot().frame.graphics.size(), 0, 5000);

    const qsizetype firstResizes = firstProcess->resizes().size();
    view.setSession(&first);
    QCoreApplication::processEvents();
    // Same grid under a new view generation: Core republishes for the new
    // epoch, but the child is not asked to resize (that forces a repaint).
    QTRY_VERIFY_WITH_TIMEOUT(gridSettled(first), 5000);
    QTest::qWait(50);
    QCOMPARE(firstProcess->resizes().size(), firstResizes);
    QTRY_COMPARE_WITH_TIMEOUT(first.latestSnapshot().frame.graphics.size(), 1, 5000);
    QVERIFY(first.latestSnapshot().frame.graphics.first().resource != nullptr);

    view.setParentItem(nullptr);
}

void TerminalSessionTest::roundTripsThroughConPty()
{
    const QString childPath = QDir(QCoreApplication::applicationDirPath())
                                  .filePath(QStringLiteral("pty_echo_child.exe"));
    QVERIFY2(QFileInfo::exists(childPath), qPrintable(childPath));
    QTemporaryDir directory;
    QVERIFY(directory.isValid());

    TerminalSession session(QStringLiteral("round-trip"), directory.path(),
                            childOptions());
    FrameConsumer consumer(session);
    QVERIFY(session.start());
    QVERIFY(waitForChildOutput(session, QStringLiteral("span_child_ready"), 8000));

    session.sendText(QStringLiteral("hello session"));
    session.sendKeyStroke(TerminalKeyStroke::named(TerminalKey::Enter));
    QTRY_VERIFY_WITH_TIMEOUT(
        frameText(session.latestSnapshot().frame).contains(
            QStringLiteral("span_child_echo=hello session")),
        8000);

    session.sendText(QStringLiteral("exit"));
    session.sendKeyStroke(TerminalKeyStroke::named(TerminalKey::Enter));
    QTRY_VERIFY_WITH_TIMEOUT(!session.running(), 8000);
    QCOMPARE(session.exitCode(), 7);
}

void TerminalSessionTest::restartsWithFreshProtocolEpoch()
{
    const QString childPath = QDir(QCoreApplication::applicationDirPath())
                                  .filePath(QStringLiteral("pty_echo_child.exe"));
    QVERIFY2(QFileInfo::exists(childPath), qPrintable(childPath));
    QTemporaryDir directory;
    QVERIFY(directory.isValid());

    TerminalSession session(QStringLiteral("restart"), directory.path(),
                            childOptions());
    FrameConsumer consumer(session);
    QSignalSpy epochChanged(&session, &TerminalSession::transportEpochChanged);
    QVERIFY(session.start());
    QVERIFY(waitForChildOutput(session, QStringLiteral("span_child_ready"), 8000));
    const quint64 firstSequence = session.latestSnapshot().sequence;

    session.stop();
    QVERIFY(session.start());
    QCOMPARE(epochChanged.count(), 1);
    QTRY_VERIFY_WITH_TIMEOUT(session.running(), 8000);
    session.sendText(QStringLiteral("second process"));
    session.sendKeyStroke(TerminalKeyStroke::named(TerminalKey::Enter));
    QTRY_VERIFY_WITH_TIMEOUT(
        frameText(session.latestSnapshot().frame).contains(
            QStringLiteral("span_child_echo=second process")),
        8000);
    QVERIFY(session.latestSnapshot().sequence > firstSequence);
    session.stop();
}

void TerminalSessionTest::keepsPreeditLocalAcrossPartialCommit()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    TerminalSession session(QStringLiteral("ime-partial-commit"), directory.path(),
                            childOptions());
    QQuickWindow window;
    window.resize(800, 240);
    TerminalView view;
    view.setParentItem(window.contentItem());
    view.setSize(QSizeF(window.width(), window.height()));
    window.show();
    QTRY_VERIFY_WITH_TIMEOUT(window.isExposed(), 5000);
    view.setSession(&session);
    QVERIFY(session.start());
    QVERIFY(waitForChildOutput(session, QStringLiteral("span_child_ready"), 8000));

    QInputMethodEvent first(QStringLiteral("pin"), {});
    QVERIFY(QCoreApplication::sendEvent(&view, &first));

    // One Windows IME event may commit a selected segment and immediately
    // continue with a fresh preedit. Both halves must be processed.
    QInputMethodEvent partial(QStringLiteral("wen"), {});
    partial.setCommitString(QStringLiteral("中"));
    QVERIFY(QCoreApplication::sendEvent(&view, &partial));

    QInputMethodEvent final(QString(), {});
    final.setCommitString(QStringLiteral("文"));
    QVERIFY(QCoreApplication::sendEvent(&view, &final));
    session.sendKeyStroke(TerminalKeyStroke::named(TerminalKey::Enter));

    QTRY_VERIFY_WITH_TIMEOUT(
        frameText(session.latestSnapshot().frame).contains(QStringLiteral("span_child_echo=中文")),
        8000);
    QVERIFY(!frameText(session.latestSnapshot().frame).contains(
        QStringLiteral("span_child_echo=pinwen中文")));
    session.stop();
}

void TerminalSessionTest::wheelScrollFinishesImeComposition()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    TerminalSession session(QStringLiteral("ime-wheel-scroll"), directory.path(),
                            childOptions());
    QQuickWindow window;
    window.resize(800, 240);
    TerminalView view;
    view.setParentItem(window.contentItem());
    view.setSize(QSizeF(window.width(), window.height()));
    window.show();
    QTRY_VERIFY_WITH_TIMEOUT(window.isExposed(), 5000);
    view.setSession(&session);
    QVERIFY(session.start());
    QVERIFY(waitForChildOutput(session, QStringLiteral("span_child_ready"), 8000));

    session.sendText(QStringLiteral("lines"));
    session.sendKeyStroke(TerminalKeyStroke::named(TerminalKey::Enter));
    QTRY_VERIFY_WITH_TIMEOUT(
        frameText(session.latestSnapshot().frame).contains(
            QStringLiteral("span_child_lines_complete")),
        8000);
    // The view scrolls over the history it presents.
    QTRY_VERIFY_WITH_TIMEOUT(view.scrollbackLines() > 0, 8000);
    // Immediate, so the offset is checked without waiting for frames.
    view.setSmoothScrolling(false);

    QInputMethodEvent preedit(QStringLiteral("pin"), {});
    QVERIFY(QCoreApplication::sendEvent(&view, &preedit));

    QWheelEvent wheel(QPointF(10, 10), QPointF(10, 10), QPoint(),
                      QPoint(0, 120), Qt::NoButton, Qt::NoModifier,
                      Qt::NoScrollPhase, false);
    QCoreApplication::sendEvent(&view, &wheel);
    QVERIFY(wheel.isAccepted());
    QVERIFY(view.scrollOffset() > 0);

    // Once scrolling terminates the composition, the next ordinary key must
    // reach the PTY instead of being swallowed as an IME continuation.
    QKeyEvent key(QEvent::KeyPress, Qt::Key_X, Qt::NoModifier,
                  QStringLiteral("x"));
    QVERIFY(QCoreApplication::sendEvent(&view, &key));
    session.sendKeyStroke(TerminalKeyStroke::named(TerminalKey::Enter));
    view.scrollToBottom();
    QTRY_VERIFY_WITH_TIMEOUT(
        frameText(session.latestSnapshot().frame).contains(QStringLiteral("span_child_echo=x")),
        8000);
    QVERIFY(!frameText(session.latestSnapshot().frame).contains(
        QStringLiteral("span_child_echo=pinx")));
    view.setParentItem(nullptr);
    session.stop();
}

void TerminalSessionTest::resolvesHiddenCursorFromRealConPtyFrame()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    TerminalSession session(QStringLiteral("ime-hidden-cursor"), directory.path(),
                            childOptions());
    FrameConsumer consumer(session);
    QVERIFY(session.start());
    QVERIFY(waitForChildOutput(session, QStringLiteral("span_child_ready"), 8000));

    session.sendText(QStringLiteral("ime-cursor"));
    session.sendKeyStroke(TerminalKeyStroke::named(TerminalKey::Enter));
    // The move and ?25l after the 2026 reset publish one frame interval
    // after the text: wait for that final frame.
    TerminalFrameSnapshot snapshot;
    const auto hiddenCursorFrameShown = [&session, &snapshot]() {
        snapshot = session.latestSnapshot();
        return frameText(snapshot.frame).contains(QStringLiteral("span_ime_redraw"))
            && !snapshot.frame.cursor.visible;
    };
    QTRY_VERIFY_WITH_TIMEOUT(hiddenCursorFrameShown(), 8000);
    const TerminalCursorResolution resolution =
        TerminalCursorResolver::resolve(snapshot.frame, snapshot.transportEpoch, true);
    QVERIFY(!resolution.visualCursor);
    QVERIFY(resolution.imeAnchor.isValid());
    QCOMPARE(resolution.imeAnchor.source,
             TerminalImeAnchorSource::ReverseVideoCell);
    QCOMPARE(resolution.imeAnchor.cursor.row, 1);
    QCOMPARE(resolution.imeAnchor.cursor.column, 2);
    session.stop();
}

void TerminalSessionTest::capturesFragmentedSynchronizedCursorInFinalFrame()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    TerminalSession session(QStringLiteral("fragmented-sync-cursor"), directory.path(),
                            childOptions());
    FrameConsumer consumer(session);
    QVERIFY(session.start());
    QVERIFY(waitForChildOutput(session, QStringLiteral("span_child_ready"), 8000));

    session.sendText(QStringLiteral("fragmented-sync-cursor"));
    session.sendKeyStroke(TerminalKeyStroke::named(TerminalKey::Enter));
    // Inbox ConPTY drops DEC 2026, so intermediate frames may appear; through
    // real ConPTY the frames only have to converge on the final cursor.
    const auto finalFrameShown = [&session]() {
        const TerminalFrameSnapshot snapshot = session.latestSnapshot();
        const QString text = frameText(snapshot.frame);
        return text.contains(QStringLiteral("span_atomic_final"))
            && text.contains(QStringLiteral("gpt-5.6-sol"))
            && snapshot.frame.cursor.row == 5 && snapshot.frame.cursor.column == 3;
    };
    QTRY_VERIFY_WITH_TIMEOUT(finalFrameShown(), 8000);
    const TerminalFrameSnapshot snapshot = session.latestSnapshot();
    QVERIFY(frameText(snapshot.frame).contains(QStringLiteral("Working")));
    QVERIFY(snapshot.frame.cursor.visible);
    session.stop();
}

QTEST_MAIN(TerminalSessionTest)

#include "terminal_session_test.moc"
