#pragma once

#include "terminal/api/terminal_types.h"
#include "terminal/api/terminal_session_input.h"
#include "terminal/terminal_constants.h"

#include <QObject>
#include <QPointer>
#include <QColor>
#include <QSize>
#include <QHash>
#include <QList>
#include <QVector>
#include <QThread>
#include <QTimer>
#include <QVariantMap>

#include <functional>
#include <memory>

namespace span::terminal {

class GhosttyEngine;
class TerminalCoreWorker;
class TerminalFrameStore;
class TerminalOutputQueue;
class TerminalProcess;

class TerminalSession final : public QObject {
    Q_OBJECT
    Q_PROPERTY(QString id READ id CONSTANT FINAL)
    Q_PROPERTY(QString title READ title NOTIFY titleChanged FINAL)
    Q_PROPERTY(bool running READ running NOTIFY stateChanged FINAL)
    Q_PROPERTY(bool failed READ failed NOTIFY stateChanged FINAL)
    Q_PROPERTY(bool starting READ starting NOTIFY stateChanged FINAL)
    Q_PROPERTY(QString startupError READ startupError NOTIFY stateChanged FINAL)
    Q_PROPERTY(bool active READ active NOTIFY activityChanged FINAL)
    Q_PROPERTY(bool hasCommandLine READ hasCommandLine CONSTANT FINAL)
    Q_PROPERTY(bool launchAttempted READ launchAttempted NOTIFY stateChanged FINAL)
    Q_PROPERTY(int exitCode READ exitCode NOTIFY stateChanged FINAL)
    // OSC 9;4 progress: 0 none, 1 normal, 2 error, 3 indeterminate, 4 paused.
    Q_PROPERTY(int progressState READ progressState NOTIFY progressChanged FINAL)
    Q_PROPERTY(int progressValue READ progressValue NOTIFY progressChanged FINAL)

public:
    enum CoreState {
        CoreStopped,
        CoreStarting,
        CoreReady,
        CoreFailed,
    };
    Q_ENUM(CoreState)

    using ProcessFactory = std::function<std::unique_ptr<TerminalProcess>()>;
    using EngineFactory = std::function<std::unique_ptr<GhosttyEngine>(int, int)>;

    TerminalSession(QString id,
                    QString cwd,
                    QVariantMap options = {},
                    QObject *parent = nullptr,
                    ProcessFactory processFactory = {},
                    EngineFactory engineFactory = {});
    ~TerminalSession() override;

    QString id() const;
    QString cwd() const;
    QString title() const;
    bool running() const;
    bool failed() const;
    bool active() const;
    bool starting() const;
    QString startupError() const { return m_startupError; }
    bool hasCommandLine() const;
    // For the next start(); a running process keeps its own.
    void setLaunchOptions(const QVariantMap &options);
    // Focus routing may auto-start only while false; an exited session needs
    // an explicit restart.
    bool launchAttempted() const;
    int columns() const;
    int rows() const;
    int exitCode() const;
    qint64 scrollbackLines() const;
    int mouseMode() const;
    CoreState coreState() const { return m_coreState; }
    qsizetype pendingOutputBytes() const;
    qsizetype pendingInputBytes() const;
    // 0 when not running.
    quint32 processId() const;
    QList<quint32> jobProcessIds() const;

    TerminalFrameSnapshot latestSnapshot() const;
    std::shared_ptr<const TerminalFrameSnapshot> latestSnapshotShared() const;
    quint64 attachRenderConsumer(QObject *owner);
    bool ownsRenderConsumer(quint64 token) const;
    void detachRenderConsumer(quint64 token);
    void setRenderConsumer(quint64 token, bool active, int maximumFramesPerSecond);
    // Viewport and selection are owned by the core; each call shows up in a
    // later snapshot. Returns the id that snapshot carries in scrollRequest,
    // 0 when nothing was queued.
    quint64 scrollViewport(quint64 token, qint64 row);
    quint64 scrollViewportToBottom(quint64 token);
    // Negative: older prompts.
    void scrollToPrompt(quint64 token, int delta);
    void selectionEvent(quint64 token, const TerminalSelectionEvent &event);
    void selectAll(quint64 token);
    void clearSelection(quint64 token);
    // Blocks until the core has applied every earlier call.
    QString selectedText() const;
    void setSearchNeedle(quint64 token, const QString &needle);
    void selectSearchMatch(quint64 token, bool newer);
    void setDefaultCursorShape(CursorShape shape);
    // Logical pixels, for XTWINOPS 14/16 replies.
    void setCellPixelSize(QSize size);
    int progressState() const { return int(m_progressState); }
    int progressValue() const { return m_progressValue; }
    // Protocol state too: TUIs query them through OSC 10/11.
    void setDefaultColors(const QColor &foreground, const QColor &background);
    void setAnsiPalette(const QVector<QColor> &palette);
    QColor defaultForeground() const { return m_defaultForeground; }
    QColor defaultBackground() const { return m_defaultBackground; }
    QVector<QColor> ansiPalette() const { return m_ansiPalette; }
    QString colorFgBg() const;
    // The notification stays reserved until this call, so a fast PTY stream
    // does not turn every output chunk into a GUI event.
    void acknowledgeFrame(quint64 token, quint64 sequence);
    void sendKeyStroke(const TerminalKeyStroke &stroke);

    Q_INVOKABLE bool start();
    // Terminates the session's process tree.
    Q_INVOKABLE void stop();
    // Product: 500 ms.
    void setRetryIntervalForTesting(int milliseconds) { m_retryTimer.setInterval(milliseconds); }
    Q_INVOKABLE void sendText(const QString &text);
    Q_INVOKABLE void paste(const QString &text);
    Q_INVOKABLE void interrupt();
    // Only a snapshot carrying the returned generation may replace the view's
    // frame.
    quint64 commitViewport(int columns, int rows);
    // Older generations are ignored. The Core resizes its grid, then the
    // child, in its own order.
    void requestViewport(int columns, int rows, quint64 viewportEpoch);
    Q_INVOKABLE void clearDisplay();
    Q_INVOKABLE void sendMouseMove(int row, int column, int modifiers = int(Qt::NoModifier));
    Q_INVOKABLE void sendMouseButton(int button,
                                     bool pressed,
                                     int row,
                                     int column,
                                     int modifiers = int(Qt::NoModifier));
    Q_INVOKABLE void setTerminalFocused(bool focused);

signals:
    void frameReady(quint64 sequence);
    void renderConsumerChanged();
    void stateChanged();
    void activityChanged();
    void titleChanged();
    // Views keep viewport state outside the engine and must reset it here.
    void transportEpochChanged();
    void errorOccurred(const QString &message);
    void bellRang();
    // OSC 9 / OSC 777; title may be empty.
    void notificationRequested(const QString &title, const QString &body);
    void progressChanged();

private:
    struct CallbackState;
    enum class Lifecycle { Stopped, StartingCore, StartingProcess, RetryWait, Running, Failed, Exited };
    void transitionTo(Lifecycle state);
    void beginStartAttempt();
    void launchProcess();
    void startPreparedProcess(quint64 generation);
    void failStart(const QString &error);
    void connectCore();
    void postInput(const std::function<void(TerminalCoreWorker *)> &operation,
                   TerminalInputKind kind = TerminalInputKind::User);
    void handleExit(quint32 exitCode, quint64 generation);
    void handleProcessStarted(const std::shared_ptr<TerminalProcess> &process, bool success, const QString &error,
                              quint64 generation, QSize size);
    void handleFrameAvailable();
    void handleTitleChanged(const QString &title);
    void handleHostEvent(const TerminalHostEvent &event);
    void setProgress(TerminalProgressState state, int value);
    void writeToPty(const QByteArray &bytes, const TerminalInputToken &token);
    void markActivity();
    void clearActivity();
    void abandonQueuedWork();
    void stopAcceptingOutput();
    void finishCoreTransport();
    void beginCoreTransportEpoch();
    void shutdownCore();
    bool ensureCoreReady();
    void postCore(const std::function<void(TerminalCoreWorker *)> &operation);

    QString m_id;
    TerminalSessionInput m_input;
    QString m_cwd;
    QString m_title;
    QString m_commandLine;
    QHash<QString, QString> m_environment;
    QColor m_defaultForeground;
    QColor m_defaultBackground;
    QVector<QColor> m_ansiPalette;
    QSize m_cellPixelSize;
    TerminalProgressState m_progressState = TerminalProgressState::None;
    int m_progressValue = 0;
    // Latest viewport the view requested; the Core and the child follow it.
    int m_columns = kDefaultColumns;
    int m_rows = kDefaultRows;
    quint64 m_viewportEpoch = 0;
    int m_exitCode = 0;
    bool m_running = false;
    bool m_processStarting = false;
    bool m_failed = false;
    bool m_active = false;
    bool m_launchAttempted = false;
    QTimer m_activityTimer;
    int m_maxScrollback = kDefaultMaxScrollbackLines;
    quint64 m_processGeneration = 0;
    quint64 m_scrollRequest = 0;
    CursorShape m_cursorShape = CursorShape::Block;
    Lifecycle m_lifecycle = Lifecycle::Stopped;
    QString m_startupError;
    QTimer m_retryTimer;
    int m_startAttempts = 0;
    std::shared_ptr<TerminalInputEpoch> m_inputEpoch;
    QPointer<QObject> m_renderConsumerOwner;
    quint64 m_renderConsumerToken = 0;
    quint64 m_nextRenderConsumerToken = 0;
    std::shared_ptr<std::atomic<quint64>> m_renderOwnership =
        std::make_shared<std::atomic<quint64>>(0);
    bool m_desiredRenderConsumerActive = false;
    int m_desiredRenderFrameRate = 60;
    bool m_coreInitializationQueued = false;
    CoreState m_coreState = CoreStopped;
    bool m_needsNewTransportEpoch = false;
    std::shared_ptr<TerminalOutputQueue> m_outputQueue;
    std::shared_ptr<TerminalFrameStore> m_frameStore;
    QThread *m_coreThread = nullptr;
    TerminalCoreWorker *m_core = nullptr;
    ProcessFactory m_processFactory;
    EngineFactory m_engineFactory;
    std::shared_ptr<CallbackState> m_callbackState;
    // Set while running. Releasing the last reference stops the process on a
    // short-lived thread of its own, so nothing waits for Windows.
    std::shared_ptr<TerminalProcess> m_process;
};

} // namespace span::terminal
