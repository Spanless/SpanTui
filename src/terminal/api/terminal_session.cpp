#include "terminal/api/terminal_session.h"
#include "terminal/core/terminal_viewport_trace.h"

#include "terminal/core/terminal_color.h"
#include "terminal/core/terminal_core_worker.h"
#include "terminal/platform/windows/win_conpty_process.h"
#include "terminal/shared/terminal_output_queue.h"
#include "terminal/shared/terminal_frame_store.h"

#include <QClipboard>
#include <QGuiApplication>
#include <QMetaObject>

#include <algorithm>
#include <mutex>
#include <system_error>
#include <thread>
#include <utility>

namespace span::terminal {

namespace {
constexpr int kActivityTimeoutMs = 2000;
constexpr int kStartRetryMs = 500;

QHash<QString, QString> environmentOption(const QVariantMap &options) {
    QHash<QString, QString> environment;
    const QVariantMap values = options.value(kSessionEnvironmentKey).toMap();
    for (auto it = values.cbegin(); it != values.cend(); ++it) {
        environment.insert(it.key(), it.value().toString());
    }
    return environment;
}

// Runs `work` on a short-lived thread of its own: start() and stop() block on
// Windows (CreatePseudoConsole, ClosePseudoConsole, a child that will not die).
void runDetached(const std::function<void()> &work)
{
    try {
        std::thread(work).detach();
    } catch (const std::system_error &) {
        qWarning("Could not start a terminal lifecycle thread; running inline.");
        work();
    }
}

// The deleter of every session process: whoever drops the last reference
// never waits for it to stop.
void retireProcess(TerminalProcess *process)
{
    if (!process) return;
    runDetached([process]() {
        process->stop();
        delete process;
    });
}
}

struct TerminalSession::CallbackState {
    std::mutex mutex;
    TerminalSession *owner = nullptr;
};

TerminalSession::TerminalSession(QString id, QString cwd, QVariantMap options, QObject *parent,
                                 ProcessFactory processFactory, EngineFactory engineFactory)
    : QObject(parent),
      m_id(std::move(id)),
      m_input(TerminalSessionInput::Sink{
                  [this](const std::function<void(TerminalCoreWorker *)> &operation, TerminalInputKind kind) {
                      postInput(operation, kind);
                  },
                  [this](const QByteArray &bytes, TerminalInputKind kind) {
                      return m_process && m_process->write(bytes, kind);
                  },
                  [this]() { return m_process ? m_process->pendingWriteBytes() : qsizetype(0); },
                  [this]() { return m_running; },
                  [this]() { return m_processStarting; },
                  [this](const QString &message) { emit errorOccurred(message); },
              },
              this),
      m_cwd(std::move(cwd)),
      m_commandLine(options.value(kSessionCommandLineKey).toString()),
      m_environment(environmentOption(options)),
      m_columns(std::max(1, options.value(kSessionColumnsKey, kDefaultColumns).toInt())),
      m_rows(std::max(1, options.value(kSessionRowsKey, kDefaultRows).toInt())),
      m_maxScrollback(options.value(kSessionMaxScrollbackKey, kDefaultMaxScrollbackLines).toInt()),
      m_outputQueue(std::make_shared<TerminalOutputQueue>()),
      m_frameStore(std::make_shared<TerminalFrameStore>()),
      m_coreThread(new QThread(this)),
      m_core(new TerminalCoreWorker(m_id, m_columns, m_rows, m_maxScrollback, m_outputQueue, m_frameStore, nullptr,
                                    engineFactory)),
      m_processFactory(std::move(processFactory)),
      m_engineFactory(std::move(engineFactory)),
      m_callbackState(std::make_shared<CallbackState>())
{
    m_callbackState->owner = this;
    if (!m_processFactory) {
        m_processFactory = []() {
            return std::make_unique<WinConPtyProcess>();
        };
    }
    connectCore();
    m_retryTimer.setSingleShot(true);
    m_retryTimer.setInterval(kStartRetryMs);
    connect(&m_retryTimer, &QTimer::timeout, this, [this]() {
        if (m_lifecycle == Lifecycle::RetryWait) beginStartAttempt();
    });

    m_activityTimer.setSingleShot(true);
    m_activityTimer.setInterval(kActivityTimeoutMs);
    connect(&m_activityTimer, &QTimer::timeout, this, [this]() {
        if (m_active) {
            m_active = false;
            emit activityChanged();
        }
    });
}

void TerminalSession::connectCore() {
    TerminalCoreWorker *core = m_core;
    m_outputQueue->setWakeCallback([core]() {
        QMetaObject::invokeMethod(core, &TerminalCoreWorker::drainOutput, Qt::QueuedConnection);
    });
    connect(core, &TerminalCoreWorker::initializationFinished, this, [this, core](bool success) {
        if (core != m_core) return;
        m_coreState = success ? CoreReady : CoreFailed;
        if (!success) {
            if (starting()) failStart(tr("Failed to initialize terminal core."));
        } else if (m_lifecycle == Lifecycle::StartingCore) {
            launchProcess();
        }
    }, Qt::QueuedConnection);
    connect(core, &TerminalCoreWorker::transportEpochFailed, this, [this, core]() {
        if (core != m_core) return;
        const QString message = tr("Failed to rebuild the terminal parser state.");
        if (starting()) {
            failStart(message);
        } else if (m_process) {
            emit errorOccurred(message);
        }
    }, Qt::QueuedConnection);
    // ConPTY keeps its own screen and only sends what changes on it: clearing
    // ours alone would leave the child's next output aimed at rows that no
    // longer hold what it expects (a vanished prompt).
    connect(
        core, &TerminalCoreWorker::displayCleared, this,
        [this, core]() {
            if (core == m_core && m_running && m_process) m_process->clearScreen();
        },
        Qt::QueuedConnection);
    connect(core, &TerminalCoreWorker::frameAvailable, this, [this, core]() {
        if (core == m_core) handleFrameAvailable();
    }, Qt::QueuedConnection);
    connect(core, &TerminalCoreWorker::outputRequested, this, &TerminalSession::writeToPty,
            Qt::QueuedConnection);
    connect(core, &TerminalCoreWorker::outputActivity, this,
            [this, core](const TerminalInputToken &token) {
        if (core == m_core && token.valid() && m_running) markActivity();
    }, Qt::QueuedConnection);
    connect(core, &TerminalCoreWorker::titleChanged, this,
            [this, core](const QString &title, const TerminalInputToken &token) {
        if (core == m_core && token.valid()) handleTitleChanged(title);
    }, Qt::QueuedConnection);
    connect(core, &TerminalCoreWorker::clipboardWriteRequested, this,
            [this, core](const QString &text, const TerminalInputToken &token) {
        if (core != m_core || !token.valid()) return;
        if (QClipboard *clipboard = QGuiApplication::clipboard()) clipboard->setText(text);
    }, Qt::QueuedConnection);
    connect(core, &TerminalCoreWorker::hostEventRequested, this,
            [this, core](const TerminalHostEvent &event, const TerminalInputToken &token) {
        if (core == m_core && token.valid()) handleHostEvent(event);
    }, Qt::QueuedConnection);
}

TerminalSession::~TerminalSession() {
    // Revoke producer callbacks before releasing the process and the core.
    // Neither waits, even if a Windows lifecycle call is stuck.
    {
        const std::lock_guard lock(m_callbackState->mutex);
        m_callbackState->owner = nullptr;
    }
    abandonQueuedWork();
    m_running = false;
    ++m_processGeneration;
    stopAcceptingOutput();
    m_process.reset();
    shutdownCore();
}

QString TerminalSession::id() const { return m_id; }
QString TerminalSession::cwd() const { return m_cwd; }
QString TerminalSession::title() const { return m_title; }
bool TerminalSession::running() const { return m_running; }
bool TerminalSession::hasCommandLine() const {
    return !m_commandLine.trimmed().isEmpty();
}
void TerminalSession::setLaunchOptions(const QVariantMap &options) {
    m_commandLine = options.value(kSessionCommandLineKey).toString();
    m_environment = environmentOption(options);
}
bool TerminalSession::launchAttempted() const { return m_launchAttempted; }
bool TerminalSession::failed() const { return m_failed; }
bool TerminalSession::active() const { return m_active; }
int TerminalSession::columns() const
{
    return m_columns;
}
int TerminalSession::rows() const
{
    return m_rows;
}
int TerminalSession::exitCode() const { return m_exitCode; }

TerminalFrameSnapshot TerminalSession::latestSnapshot() const {
    const auto snapshot = latestSnapshotShared();
    return snapshot ? *snapshot : TerminalFrameSnapshot{};
}

std::shared_ptr<const TerminalFrameSnapshot> TerminalSession::latestSnapshotShared() const {
    return m_frameStore ? m_frameStore->latestShared() : nullptr;
}

qint64 TerminalSession::scrollbackLines() const
{
    const auto snapshot = latestSnapshotShared();
    if (!snapshot) return 0;
    const TerminalScrollbar &scrollbar = snapshot->frame.scrollbar;
    return std::max<qint64>(0, scrollbar.total - scrollbar.rows);
}

int TerminalSession::mouseMode() const {
    const auto snapshot = latestSnapshotShared();
    return snapshot ? snapshot->mouseMode : 0;
}

qsizetype TerminalSession::pendingOutputBytes() const {
    return m_outputQueue ? m_outputQueue->size() : 0;
}

qsizetype TerminalSession::pendingInputBytes() const {
    return m_process ? m_process->pendingWriteBytes() : 0;
}

quint32 TerminalSession::processId() const {
    return m_running && m_process ? m_process->processId() : 0;
}

QList<quint32> TerminalSession::jobProcessIds() const {
    return m_running && m_process ? m_process->jobProcessIds() : QList<quint32>{};
}

quint64 TerminalSession::attachRenderConsumer(QObject *owner) {
    if (!owner || owner->thread() != thread()) return 0;
    const quint64 token = ++m_nextRenderConsumerToken;
    m_renderConsumerOwner = owner;
    m_renderConsumerToken = token;
    m_renderOwnership->store(token, std::memory_order_release);
    connect(owner, &QObject::destroyed, this, [this, token]() { detachRenderConsumer(token); });
    setRenderConsumer(token, false, 60);
    const auto snapshot = latestSnapshotShared();
    if (snapshot) acknowledgeFrame(token, snapshot->sequence);
    emit renderConsumerChanged();
    return token;
}

bool TerminalSession::ownsRenderConsumer(quint64 token) const {
    return token != 0 && token == m_renderConsumerToken && m_renderConsumerOwner;
}

void TerminalSession::detachRenderConsumer(quint64 token) {
    if (!token || token != m_renderConsumerToken) return;
    m_renderConsumerToken = 0;
    m_renderConsumerOwner.clear();
    m_renderOwnership->store(0, std::memory_order_release);
    m_desiredRenderConsumerActive = false;
    const auto ownership = m_renderOwnership;
    postCore([ownership](TerminalCoreWorker *core) {
        if (ownership->load(std::memory_order_acquire) == 0)
            core->setRenderConsumer(false, 60);
    });
    emit renderConsumerChanged();
}

quint64 TerminalSession::scrollViewport(quint64 token, qint64 row)
{
    if (!ownsRenderConsumer(token) || !ensureCoreReady()) return 0;
    const quint64 request = ++m_scrollRequest;
    postCore([row, request](TerminalCoreWorker *core) { core->scrollViewport(row, request); });
    return request;
}

quint64 TerminalSession::scrollViewportToBottom(quint64 token)
{
    if (!ownsRenderConsumer(token) || !ensureCoreReady()) return 0;
    const quint64 request = ++m_scrollRequest;
    postCore([request](TerminalCoreWorker *core) { core->scrollViewportToBottom(request); });
    return request;
}

void TerminalSession::scrollToPrompt(quint64 token, int delta)
{
    if (!ownsRenderConsumer(token) || !ensureCoreReady()) return;
    postCore([delta](TerminalCoreWorker *core) { core->scrollToPrompt(delta); });
}

void TerminalSession::selectionEvent(quint64 token, const TerminalSelectionEvent &event)
{
    if (!ownsRenderConsumer(token) || !ensureCoreReady()) return;
    postCore([event](TerminalCoreWorker *core) { core->selectionEvent(event); });
}

void TerminalSession::selectAll(quint64 token)
{
    if (!ownsRenderConsumer(token) || !ensureCoreReady()) return;
    postCore([](TerminalCoreWorker *core) { core->selectAll(); });
}

void TerminalSession::clearSelection(quint64 token)
{
    if (!ownsRenderConsumer(token) || !ensureCoreReady()) return;
    postCore([](TerminalCoreWorker *core) { core->clearSelection(); });
}

QString TerminalSession::selectedText() const
{
    if (!m_core || !m_coreThread || !m_coreThread->isRunning()) return {};
    TerminalCoreWorker *core = m_core;
    if (QThread::currentThread() == m_coreThread) return core->selectedText();
    QString text;
    QMetaObject::invokeMethod(core, [core, &text]() { text = core->selectedText(); }, Qt::BlockingQueuedConnection);
    return text;
}

void TerminalSession::setSearchNeedle(quint64 token, const QString &needle)
{
    if (!ownsRenderConsumer(token) || !ensureCoreReady()) return;
    postCore([needle](TerminalCoreWorker *core) { core->setSearchNeedle(needle); });
}

void TerminalSession::selectSearchMatch(quint64 token, bool newer)
{
    if (!ownsRenderConsumer(token) || !ensureCoreReady()) return;
    postCore([newer](TerminalCoreWorker *core) { core->selectSearchMatch(newer); });
}

void TerminalSession::setDefaultCursorShape(CursorShape shape)
{
    if (m_cursorShape == shape) return;
    m_cursorShape = shape;
    if (m_coreThread && m_coreThread->isRunning()) {
        postCore([shape](TerminalCoreWorker *core) { core->setDefaultCursorShape(shape); });
        return;
    }
    if (m_core) m_core->setDefaultCursorShape(shape);
}

void TerminalSession::setCellPixelSize(QSize size) {
    if (m_cellPixelSize == size) {
        return;
    }
    m_cellPixelSize = size;
    if (m_coreThread && m_coreThread->isRunning()) {
        postCore([size](TerminalCoreWorker *core) { core->setCellPixelSize(size); });
        return;
    }
    if (m_core) {
        m_core->setCellPixelSize(size);
    }
}

void TerminalSession::setRenderConsumer(quint64 token, bool active, int maximumFramesPerSecond) {
    if (!ownsRenderConsumer(token)) return;
    m_desiredRenderConsumerActive = active;
    m_desiredRenderFrameRate = maximumFramesPerSecond;
    if (active && !ensureCoreReady()) return;
    const auto ownership = m_renderOwnership;
    postCore([ownership, token, active, maximumFramesPerSecond](TerminalCoreWorker *core) {
        if (ownership->load(std::memory_order_acquire) == token)
            core->setRenderConsumer(active, maximumFramesPerSecond);
    });
}

void TerminalSession::setDefaultColors(const QColor &foreground, const QColor &background) {
    if (m_defaultForeground == foreground && m_defaultBackground == background) {
        return;
    }
    m_defaultForeground = foreground;
    m_defaultBackground = background;
    if (m_coreThread && m_coreThread->isRunning()) {
        postCore([foreground, background](TerminalCoreWorker *core) {
            core->setDefaultColors(foreground, background);
        });
        return;
    }
    if (m_core) {
        m_core->setDefaultColors(foreground, background);
    }
}

void TerminalSession::setAnsiPalette(const QVector<QColor> &palette) {
    if (palette.size() != 16 || m_ansiPalette == palette) {
        return;
    }
    m_ansiPalette = palette;
    if (m_coreThread && m_coreThread->isRunning()) {
        postCore([palette](TerminalCoreWorker *core) {
            core->setAnsiPalette(palette);
        });
        return;
    }
    if (m_core) {
        m_core->setAnsiPalette(palette);
    }
}

QString TerminalSession::colorFgBg() const {
    return span::terminal::colorFgBg(m_defaultForeground, m_defaultBackground);
}

void TerminalSession::acknowledgeFrame(quint64 token, quint64 sequence) {
    if (!ownsRenderConsumer(token) || !m_frameStore || sequence == 0) return;
    if (m_frameStore->acknowledgeNotification(sequence) == FrameNotificationDisposition::NotifyAgain)
        QMetaObject::invokeMethod(this, &TerminalSession::handleFrameAvailable, Qt::QueuedConnection);
}


bool TerminalSession::ensureCoreReady() {
    if (!m_core || !m_coreThread) {
        return false;
    }
    if (m_coreState == CoreFailed) {
        return false;
    }
    if (!m_coreThread->isRunning()) {
        // QObject::moveToThread() must be issued by the object's current
        // owner. Session APIs are GUI-thread APIs, so reject an accidental
        // cross-thread first start instead of silently creating bad affinity.
        if (QThread::currentThread() != thread() || m_core->thread() != thread()) {
            return false;
        }
        m_core->moveToThread(m_coreThread);
        m_coreThread->start();
    }
    if (!m_coreInitializationQueued) {
        m_coreInitializationQueued = true;
        m_coreState = CoreStarting;
        if (QThread::currentThread() == m_coreThread) {
            m_core->initialize();
        } else {
            QMetaObject::invokeMethod(m_core,
                                      &TerminalCoreWorker::initialize,
                                      Qt::QueuedConnection);
        }
    }
    return true;
}

void TerminalSession::postCore(const std::function<void(TerminalCoreWorker *)> &operation) {
    if (!m_core || !m_coreThread || !m_coreThread->isRunning()) {
        return;
    }
    TerminalCoreWorker *core = m_core;
    QMetaObject::invokeMethod(core,
                              [core, operation]() { operation(core); },
                              Qt::QueuedConnection);
}

bool TerminalSession::starting() const {
    return m_lifecycle == Lifecycle::StartingCore || m_lifecycle == Lifecycle::StartingProcess
        || m_lifecycle == Lifecycle::RetryWait;
}

void TerminalSession::transitionTo(Lifecycle state) {
    m_lifecycle = state;
    m_running = state == Lifecycle::Running;
    m_processStarting = state == Lifecycle::StartingProcess;
    m_failed = state == Lifecycle::Failed || (state == Lifecycle::Exited && m_exitCode != 0);
    emit stateChanged();
}

void TerminalSession::postInput(const std::function<void(TerminalCoreWorker *)> &operation,
                                TerminalInputKind kind) {
    if (!m_inputEpoch) return;
    const TerminalInputToken token{m_inputEpoch,
        m_inputEpoch->userSerial.load(std::memory_order_acquire), kind};
    postCore([token, operation](TerminalCoreWorker *core) { core->executeInput(token, operation); });
}

bool TerminalSession::start() {
    if (m_running || starting()) return true;
    m_launchAttempted = true;
    m_startAttempts = 0;
    m_startupError.clear();
    beginStartAttempt();
    return starting() || m_running;
}

void TerminalSession::beginStartAttempt() {
    ++m_startAttempts;
    ++m_processGeneration;
    if (m_coreState == CoreFailed) {
        shutdownCore();
        m_outputQueue = std::make_shared<TerminalOutputQueue>();
        m_frameStore = std::make_shared<TerminalFrameStore>();
        m_coreThread = new QThread(this);
        m_core = new TerminalCoreWorker(m_id, columns(), rows(), m_maxScrollback,
                                        m_outputQueue, m_frameStore, nullptr, m_engineFactory);
        m_core->setDefaultColors(m_defaultForeground, m_defaultBackground);
        m_core->setAnsiPalette(m_ansiPalette);
        m_core->setCellPixelSize(m_cellPixelSize);
        m_core->setDefaultCursorShape(m_cursorShape);
        m_coreInitializationQueued = false;
        m_coreState = CoreStopped;
        // The rebuilt core starts from a fresh engine; another epoch would
        // only replace it with a second fresh one.
        m_needsNewTransportEpoch = false;
        connectCore();
    }
    transitionTo(Lifecycle::StartingCore);
    if (!ensureCoreReady()) {
        failStart(tr("Failed to initialize terminal core."));
        return;
    }
    if (m_coreState == CoreReady) launchProcess();
}

void TerminalSession::failStart(const QString &error) {
    ++m_processGeneration;
    m_startupError = error;
    abandonQueuedWork();
    stopAcceptingOutput();
    m_process.reset();
    finishCoreTransport();
    m_needsNewTransportEpoch = true;
    transitionTo(m_startAttempts < 3 ? Lifecycle::RetryWait : Lifecycle::Failed);
    if (m_lifecycle == Lifecycle::RetryWait) m_retryTimer.start();
    else emit errorOccurred(error);
}

void TerminalSession::launchProcess()
{
    handleTitleChanged(QString());
    if (m_lifecycle != Lifecycle::StartingCore || m_coreState != CoreReady) return;
    if (m_needsNewTransportEpoch) {
        beginCoreTransportEpoch();
        m_needsNewTransportEpoch = false;
    }
    m_inputEpoch = std::make_shared<TerminalInputEpoch>();
    const auto callbackState = m_callbackState;
    const quint64 generation = m_processGeneration;
    const auto inputEpoch = m_inputEpoch;
    const int columns = m_columns;
    const int rows = m_rows;
    const quint64 epoch = m_viewportEpoch;
    transitionTo(Lifecycle::StartingProcess);
    // Through the Core first: the previous child's tail must be drained
    // before the output queue opens for the next one.
    postCore([inputEpoch, callbackState, generation, columns, rows, epoch](TerminalCoreWorker *core) {
        if (!inputEpoch->active.load(std::memory_order_acquire)) return;
        core->setInputEpoch(inputEpoch);
        core->resize(columns, rows, epoch);
        const std::lock_guard lock(callbackState->mutex);
        TerminalSession *session = callbackState->owner;
        if (session)
            QMetaObject::invokeMethod(
                session, [session, generation]() { session->startPreparedProcess(generation); }, Qt::QueuedConnection);
    });
}

void TerminalSession::startPreparedProcess(quint64 generation)
{
    if (generation != m_processGeneration || m_lifecycle != Lifecycle::StartingProcess) return;
    const auto inputEpoch = m_inputEpoch;
    if (m_outputQueue && m_outputQueue->open())
        postCore([](TerminalCoreWorker *core) { core->drainOutput(); });
    if (ownsRenderConsumer(m_renderConsumerToken))
        setRenderConsumer(m_renderConsumerToken, m_desiredRenderConsumerActive, m_desiredRenderFrameRate);
    const auto outputQueue = m_outputQueue;
    const auto callbackState = m_callbackState;
    TerminalProcess::StartOptions options;
    options.commandLine = m_commandLine;
    options.workingDirectory = m_cwd;
    options.columns = m_columns;
    options.rows = m_rows;
    options.colorFgBg = colorFgBg();
    options.environment = m_environment;

    TerminalViewportTrace::record(m_id, QStringLiteral("process-start"),
                                  {{QStringLiteral("columns"), options.columns},
                                   {QStringLiteral("rows"), options.rows},
                                   {QStringLiteral("epoch"), qint64(m_viewportEpoch)}});
    TerminalProcess::OutputCallback output = [outputQueue, inputEpoch](const QByteArray &bytes, bool endsWrite) {
        const std::lock_guard lock(inputEpoch->outputMutex);
        if (outputQueue && inputEpoch->active.load(std::memory_order_acquire)) outputQueue->append(bytes, endsWrite);
    };
    TerminalProcess::ExitCallback exit = [callbackState, generation](quint32 exitCode) {
        const std::lock_guard lock(callbackState->mutex);
        if (TerminalSession *session = callbackState->owner) {
            QMetaObject::invokeMethod(
                session, [session, exitCode, generation]() { session->handleExit(exitCode, generation); },
                Qt::QueuedConnection);
        }
    };
    runDetached([factory = m_processFactory, options, output, exit, callbackState, generation]() {
        const std::shared_ptr<TerminalProcess> process(factory ? factory().release() : nullptr, &retireProcess);
        QString error = QStringLiteral("Terminal process factory returned null.");
        const bool started = process && process->start(options, output, exit, &error);
        const QSize size(options.columns, options.rows);
        const std::lock_guard lock(callbackState->mutex);
        // A session that is gone drops the event, and with it the process.
        if (TerminalSession *session = callbackState->owner) {
            QMetaObject::invokeMethod(
                session,
                [session, process, started, error, generation, size]() {
                    session->handleProcessStarted(process, started, error, generation, size);
                },
                Qt::QueuedConnection);
        }
    });
}

void TerminalSession::handleProcessStarted(const std::shared_ptr<TerminalProcess> &process, bool success,
                                           const QString &error, quint64 generation, QSize size)
{
    if (generation != m_processGeneration || m_lifecycle != Lifecycle::StartingProcess || m_coreState != CoreReady)
        return;
    if (!success) {
        failStart(tr("Failed to start terminal: %1").arg(error));
        return;
    }
    m_process = process;
    // The Core resizes the child to any viewport committed while it started.
    postCore([process, size](TerminalCoreWorker *core) { core->attachProcess(process, size.width(), size.height()); });
    m_startupError.clear();
    m_exitCode = 0;
    transitionTo(Lifecycle::Running);
}

void TerminalSession::stop() {
    const bool wasRunning = m_running || starting();
    m_retryTimer.stop();
    abandonQueuedWork();
    ++m_processGeneration;
    transitionTo(Lifecycle::Stopped);

    stopAcceptingOutput();
    m_process.reset();
    finishCoreTransport();
    if (wasRunning)
        m_needsNewTransportEpoch = true;
    clearActivity();
}

void TerminalSession::sendText(const QString &text) {
    if (m_running && !text.isEmpty()) {
        markActivity();
        postInput([text](TerminalCoreWorker *core) { core->sendText(text); });
    }
}

void TerminalSession::sendKeyStroke(const TerminalKeyStroke &stroke) {
    if (m_running) {
        markActivity();
        postInput([stroke](TerminalCoreWorker *core) { core->sendKeyStroke(stroke); });
    }
}

void TerminalSession::paste(const QString &text) {
    if (!m_running || text.isEmpty()) {
        return;
    }
    markActivity();
    m_input.paste(text);
}

void TerminalSession::interrupt() {
    if (!m_running || !m_process) {
        return;
    }
    markActivity();
    m_inputEpoch->userSerial.fetch_add(1, std::memory_order_acq_rel);
    // cancelQueued() closes an open bracketed-paste context itself.
    m_input.cancelQueued(false);
    if (!m_process->cancelPendingWritesAndInterrupt()) {
        emit errorOccurred(tr("Could not deliver interrupt to the terminal process."));
    }
}

quint64 TerminalSession::commitViewport(int columns, int rows)
{
    const quint64 generation = m_viewportEpoch + 1;
    requestViewport(columns, rows, generation);
    return generation;
}

void TerminalSession::requestViewport(int columns, int rows,
                                      quint64 viewportEpoch) {
    columns = std::max(1, columns);
    rows = std::max(1, rows);
    if (viewportEpoch < m_viewportEpoch
        || (viewportEpoch == m_viewportEpoch && columns == m_columns && rows == m_rows)) {
        return;
    }
    m_columns = columns;
    m_rows = rows;
    m_viewportEpoch = viewportEpoch;
    TerminalViewportTrace::record(m_id, QStringLiteral("viewport-request"),
                                  {{QStringLiteral("columns"), columns},
                                   {QStringLiteral("rows"), rows},
                                   {QStringLiteral("epoch"), qint64(viewportEpoch)}});
    if (!ensureCoreReady()) return;
    postCore([columns, rows, viewportEpoch](TerminalCoreWorker *core) { core->resize(columns, rows, viewportEpoch); });
}

void TerminalSession::clearDisplay() {
    if (ensureCoreReady()) {
        postCore([](TerminalCoreWorker *core) { core->clearDisplay(); });
    }
}

void TerminalSession::sendMouseMove(int row, int column, int modifiers) {
    if (m_running) {
        postInput([row, column, modifiers](TerminalCoreWorker *core) {
            core->sendMouseMove(row, column, modifiers);
        });
    }
}

void TerminalSession::sendMouseButton(int button,
                                      bool pressed,
                                      int row,
                                      int column,
                                      int modifiers) {
    if (m_running && button >= int(TerminalMouseButton::Left) &&
        button <= int(TerminalMouseButton::WheelRight)) {
        postInput([button, pressed, row, column, modifiers](TerminalCoreWorker *core) {
            core->sendMouseButton(button, pressed, row, column, modifiers);
        });
    }
}

void TerminalSession::setTerminalFocused(bool focused) {
    if (m_running) {
        postInput([focused](TerminalCoreWorker *core) { core->setFocused(focused); });
    }
}

void TerminalSession::handleFrameAvailable() {
    if (!m_frameStore) {
        return;
    }
    const auto snapshot = m_frameStore->latestShared();
    if (!snapshot || snapshot->sequence == 0) {
        return;
    }
    emit frameReady(snapshot->sequence);
}

void TerminalSession::handleTitleChanged(const QString &title) {
    if (m_title == title) {
        return;
    }
    m_title = title;
    emit titleChanged();
}

void TerminalSession::handleHostEvent(const TerminalHostEvent &event) {
    switch (event.kind) {
    case TerminalHostEvent::Kind::Bell:
        emit bellRang();
        break;
    case TerminalHostEvent::Kind::Notification:
        emit notificationRequested(event.title, event.text);
        break;
    case TerminalHostEvent::Kind::Progress:
        setProgress(event.progress, event.progressValue);
        break;
    }
}

void TerminalSession::setProgress(TerminalProgressState state, int value) {
    if (state == TerminalProgressState::None) value = 0;
    if (m_progressState == state && m_progressValue == value) {
        return;
    }
    m_progressState = state;
    m_progressValue = value;
    emit progressChanged();
}

void TerminalSession::handleExit(quint32 exitCode, quint64 generation) {
    if (generation != m_processGeneration || (!m_running && !m_processStarting)) return;
    ++m_processGeneration;
    m_exitCode = int(exitCode);
    transitionTo(Lifecycle::Exited);
    abandonQueuedWork();
    // Progress belongs to the child that reported it.
    setProgress(TerminalProgressState::None, 0);
    m_process.reset();
    stopAcceptingOutput();
    finishCoreTransport();
    m_needsNewTransportEpoch = true;
    clearActivity();
}

void TerminalSession::writeToPty(const QByteArray &bytes, const TerminalInputToken &token) {
    m_input.writeFromEngine(bytes, token);
}


void TerminalSession::markActivity() {
    if (!m_active) {
        m_active = true;
        emit activityChanged();
    }
    m_activityTimer.start();
}

void TerminalSession::clearActivity() {
    if (!m_active) {
        return;
    }
    m_active = false;
    m_activityTimer.stop();
    emit activityChanged();
}

void TerminalSession::abandonQueuedWork() {
    if (m_inputEpoch) {
        const std::lock_guard lock(m_inputEpoch->outputMutex);
        m_inputEpoch->active.store(false, std::memory_order_release);
    }
    m_input.cancelQueued();
}

void TerminalSession::stopAcceptingOutput() {
    if (m_outputQueue) {
        m_outputQueue->close();
    }
}

void TerminalSession::finishCoreTransport() {
    if (!m_core || !m_coreThread || !m_coreThread->isRunning()) {
        return;
    }
    postCore([](TerminalCoreWorker *core) { core->finishTransport(); });
}

void TerminalSession::beginCoreTransportEpoch() {
    if (!m_core || !m_coreThread || !m_coreThread->isRunning())
        return;
    postCore([](TerminalCoreWorker *core) { core->beginTransportEpoch(); });
    emit transportEpochChanged();
}


void TerminalSession::shutdownCore() {
    if (!m_core) {
        return;
    }
    disconnect(m_core, nullptr, this, nullptr);
    if (m_outputQueue) {
        m_outputQueue->close();
        // Waits for any wake callback currently executing in append(), then
        // guarantees that future PTY callbacks cannot touch m_core.
        m_outputQueue->clearWakeCallback();
    }
    TerminalCoreWorker *core = m_core;
    QThread *coreThread = m_coreThread;
    m_core = nullptr;
    m_coreThread = nullptr;
    if (!coreThread || !coreThread->isRunning()) {
        delete core;
        delete coreThread;
        return;
    }

    // Session teardown is a one-way ownership handoff. A wedged core cannot
    // make the GUI wait; the isolated thread destroys itself after its queued
    // shutdown reaches the event loop.
    coreThread->setParent(nullptr);
    connect(coreThread, &QThread::finished,
            core, &QObject::deleteLater);
    connect(coreThread, &QThread::finished,
            coreThread, &QObject::deleteLater);
    QMetaObject::invokeMethod(
        core, [core, coreThread]() {
            core->shutdown();
            coreThread->quit();
        }, Qt::QueuedConnection);
}

} // namespace span::terminal
