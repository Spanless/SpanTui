#pragma once

#include "terminal/api/terminal_types.h"
#include "terminal/api/terminal_input_transaction.h"
#include "terminal/core/terminal_presentation_scheduler.h"
#include "terminal/terminal_constants.h"

#include <QByteArray>
#include <QByteArrayView>
#include <QColor>
#include <QSize>
#include <QVector>
#include <QElapsedTimer>
#include <QObject>

#include <functional>
#include <memory>
#include <optional>

class QTimer;

namespace span::terminal {

class GhosttyEngine;
class TerminalOutputQueue;
class TerminalFrameStore;
class TerminalProcess;

// Begin/End are DECSET/DECRST 2026. RedrawBegin is ED3 (CSI 3 J): the child
// erased its scrollback and is about to reprint everything, usually without
// declaring a 2026 transaction around it.
enum class SynchronizedUpdateBoundary : quint8 { None, Begin, End, RedrawBegin };

struct SynchronizedUpdateSegment {
    QByteArray bytes;
    SynchronizedUpdateBoundary boundaryAfter = SynchronizedUpdateBoundary::None;
    // DECSET 9001 (win32-input-mode): libghostty does not know the mode.
    std::optional<bool> win32InputAfter;
    // XTMODKEYS 4 (CSI > 4 ; n m); -1 when unchanged.
    int modifyOtherKeysAfter = -1;
};

// Observes without removing bytes. Only one bounded, unfinished CSI is kept
// between calls, so malformed input cannot grow its state.
class SynchronizedUpdateParser final {
public:
    QVector<SynchronizedUpdateSegment> push(QByteArrayView bytes);
    QByteArray flushPending();

private:
    QByteArray m_candidate;
};

class TerminalCoreWorker final : public QObject {
    Q_OBJECT

public:
    // Test seam.
    using EngineFactory = std::function<std::unique_ptr<GhosttyEngine>(int, int)>;
    TerminalCoreWorker(QString sessionId,
                       int columns,
                       int rows,
                       int maxScrollback,
                       std::shared_ptr<TerminalOutputQueue> outputQueue,
                       std::shared_ptr<TerminalFrameStore> frameStore,
                       QObject *parent = nullptr, EngineFactory engineFactory = {});
    ~TerminalCoreWorker() override;
    void setInputEpoch(std::shared_ptr<TerminalInputEpoch> epoch);
    void executeInput(const TerminalInputToken &token,
                      const std::function<void(TerminalCoreWorker *)> &operation);

public slots:
    void initialize();
    void shutdown();
    // The started child, created at `columns` x `rows`: every later grid
    // change reaches it right after the grid is resized, so its repaint is
    // parsed on the grid it was drawn for.
    void attachProcess(std::shared_ptr<span::terminal::TerminalProcess> process, int columns, int rows);
    // Only once no producer can append any more. Releases the process.
    void finishTransport();
    // A new engine: nothing of the previous child carries over.
    void beginTransportEpoch();
    void drainOutput();
    void setRenderConsumer(bool active, int maximumFramesPerSecond);
    void setDefaultColors(const QColor &foreground, const QColor &background);
    void setAnsiPalette(const QVector<QColor> &palette);
    void setDefaultCursorShape(span::terminal::CursorShape shape);
    void setCellPixelSize(QSize size);
    // A grid change coalesces output-driven frames until the child's resize
    // repaint settles.
    void resize(int columns, int rows, quint64 viewportEpoch = 0);
    void clearDisplay();
    void sendKeyStroke(const TerminalKeyStroke &stroke);
    void sendText(const QString &text);
    void beginPaste();
    void endPaste();
    void sendPasteBody(const QString &text);
    void sendMouseMove(int row, int column, int modifiers);
    void sendMouseButton(int button, bool pressed, int row, int column, int modifiers);
    void setFocused(bool focused);

    // `request` is echoed in the snapshot that shows it.
    void scrollViewport(qint64 row, quint64 request);
    void scrollViewportToBottom(quint64 request);
    void scrollToPrompt(int delta);
    void selectionEvent(const span::terminal::TerminalSelectionEvent &event);
    void selectAll();
    void clearSelection();
    // Synchronous: the GUI asks for it with a blocking queued call.
    QString selectedText() const;
    // Matches are counted in bounded steps between output chunks.
    void setSearchNeedle(const QString &needle);
    void selectSearchMatch(bool newer);

signals:
    void initializationFinished(bool success);
    void transportEpochFailed();
    // The backend's copy of the screen is cleared next.
    void displayCleared();
    void frameAvailable();
    void outputRequested(const QByteArray &bytes, const TerminalInputToken &token);
    void outputActivity(const TerminalInputToken &token);
    void titleChanged(const QString &title, const TerminalInputToken &token);
    void clipboardWriteRequested(const QString &text, const TerminalInputToken &token);
    void hostEventRequested(const span::terminal::TerminalHostEvent &event,
                            const TerminalInputToken &token);

private:
    void createEngine();
    void processOutput(const QByteArray &bytes);
    void handleBoundary(SynchronizedUpdateBoundary boundary);
    qint64 nowMs() const { return m_clock.elapsed(); }
    void markDirty() { m_scheduler.markDirty(nowMs()); }
    void requestPresentation(bool immediate);
    // A due frame is published from the present timer, or here when
    // `publishInline`.
    void schedulePresentation(bool publishInline = false);
    void publishSnapshot();
    bool ensureInitialized();
    int frameIntervalMs() const;
    void followInput();
    void scheduleSearchStep();
    void searchStep();
    void autoscrollTick();
    // Narrow while scroll requests jump past the overscan (a dragged
    // scrollbar), wide again once they settle.
    void adaptOverscan(bool jumped);

    QString m_sessionId;
    int m_columns = kDefaultColumns;
    int m_rows = kDefaultRows;
    int m_maxScrollback = kDefaultMaxScrollbackLines;
    int m_maximumFramesPerSecond = kDefaultFrameRate;
    quint64 m_sequence = 0;
    quint64 m_transportEpoch = 1;
    quint64 m_viewportEpoch = 0;
    quint64 m_scrollRequest = 0;
    quint64 m_viewportJump = 0;
    // Bumped by whatever invalidates every prepared row: resize, new
    // transport, new render consumer.
    quint64 m_liveFullRefreshRevision = 1;
    quint64 m_lastPublishedFullRefreshRevision = 0;
    std::shared_ptr<TerminalOutputQueue> m_outputQueue;
    std::shared_ptr<TerminalFrameStore> m_frameStore;
    QColor m_defaultForeground;
    QColor m_defaultBackground;
    QSize m_cellPixelSize;
    QVector<QColor> m_ansiPalette;
    CursorShape m_cursorShape = CursorShape::Block;
    QString m_searchNeedle;
    bool m_searchRevealPending = false;
    std::unique_ptr<GhosttyEngine> m_engine;
    EngineFactory m_engineFactory;
    std::shared_ptr<TerminalProcess> m_process;
    QTimer *m_presentTimer = nullptr;
    QTimer *m_searchTimer = nullptr;
    QTimer *m_autoscrollTimer = nullptr;
    QTimer *m_compressTimer = nullptr;
    QTimer *m_overscanTimer = nullptr;
    QElapsedTimer m_clock;
    QElapsedTimer m_activityClock;
    TerminalPresentationScheduler m_scheduler;
    SynchronizedUpdateParser m_protocolParser;
    bool m_initialized = false;
    bool m_initializationFailed = false;
    bool m_shuttingDown = false;
    bool m_rendererActive = false;
    int m_viewportTraceFrames = 8;
    quint64 m_receivedOutputBytes = 0;
    // Publications that cut a producer write short (diagnostics).
    quint64 m_forcedCuts = 0;
    bool m_pendingForcedCut = false;
    bool m_pasteActive = false;
    TerminalInputToken m_inputToken;
};

} // namespace span::terminal
