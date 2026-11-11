#include "terminal/core/terminal_core_worker.h"

#include "terminal/api/terminal_process.h"
#include "terminal/shared/terminal_output_queue.h"
#include "terminal/shared/terminal_frame_store.h"
#include "terminal/core/ghostty_engine.h"
#include "terminal/core/terminal_viewport_trace.h"
#include "terminal/terminal_constants.h"

#include <QLoggingCategory>
#include <QTimer>

#include <algorithm>
#include <cmath>
#include <utility>
#include <stdexcept>

namespace span::terminal {

namespace {
constexpr qsizetype kWorkerOutputSliceBytes = 16 * 1024;
constexpr qsizetype kWorkerOutputBudgetBytes = 64 * 1024;
constexpr qint64 kWorkerOutputBudgetMs = 2;
constexpr qint64 kActivitySignalIntervalMs = 250;
constexpr qsizetype kMaximumObservedCsiBytes = 256;
// Selection drag past the viewport edge: one row per tick.
constexpr int kAutoscrollIntervalMs = 50;
// History pages are compressed once output has been quiet this long.
constexpr int kCompressIdleMs = 10'000;
// Scroll jumps this long apart count as one drag; the overscan stays narrow.
constexpr int kOverscanSettleMs = 150;
Q_LOGGING_CATEGORY(terminalProtocolLog, "span.terminal.protocol", QtWarningMsg)

bool isCsiFinal(char byte) {
    const uchar value = uchar(byte);
    return value >= 0x40 && value <= 0x7e;
}

bool isCsiBody(char byte) {
    const uchar value = uchar(byte);
    return value >= 0x20 && value <= 0x3f;
}

// What the worker needs to know about one complete CSI.
void observeCsi(const QByteArray &csi, SynchronizedUpdateSegment *segment)
{
    if (csi == QByteArrayView("\x1b[3J")) {
        segment->boundaryAfter = SynchronizedUpdateBoundary::RedrawBegin;
        return;
    }
    if (csi.size() < 4 || !csi.startsWith("\x1b[")) return;
    const char final = csi.back();
    const QByteArray body = csi.mid(2, csi.size() - 3);
    if (body.startsWith('?') && (final == 'h' || final == 'l')) {
        const QList<QByteArray> parameters = body.sliced(1).split(';');
        const bool set = final == 'h';
        if (parameters.contains(QByteArrayLiteral("2026"))) {
            segment->boundaryAfter = set ? SynchronizedUpdateBoundary::Begin : SynchronizedUpdateBoundary::End;
        }
        if (parameters.contains(QByteArrayLiteral("9001"))) segment->win32InputAfter = set;
        return;
    }
    // XTMODKEYS: CSI > 4 ; n m sets modifyOtherKeys, CSI > 4 m and
    // CSI > 4 n reset it.
    if (body.startsWith('>') && (final == 'm' || final == 'n')) {
        const QList<QByteArray> parameters = body.sliced(1).split(';');
        if (parameters.value(0) != QByteArrayLiteral("4")) return;
        segment->modifyOtherKeysAfter =
            final == 'm' && parameters.size() > 1 ? std::max(0, parameters.at(1).toInt()) : 0;
    }
}

} // namespace

QVector<SynchronizedUpdateSegment> SynchronizedUpdateParser::push(QByteArrayView bytes) {
    QVector<SynchronizedUpdateSegment> result;
    QByteArray ordinary;
    auto flush = [&](const SynchronizedUpdateSegment &observed) {
        const bool event = observed.boundaryAfter != SynchronizedUpdateBoundary::None || observed.win32InputAfter
            || observed.modifyOtherKeysAfter >= 0;
        if (!ordinary.isEmpty() || event) {
            SynchronizedUpdateSegment segment = observed;
            segment.bytes = std::exchange(ordinary, {});
            result.push_back(std::move(segment));
        }
    };

    for (char byte : bytes) {
        bool reprocess = true;
        while (reprocess) {
            reprocess = false;
            if (m_candidate.isEmpty()) {
                if (byte == '\x1b') {
                    m_candidate.append(byte);
                } else {
                    ordinary.append(byte);
                }
                continue;
            }

            if (m_candidate.size() == 1) {
                if (byte == '[') {
                    m_candidate.append(byte);
                } else {
                    ordinary.append(m_candidate);
                    m_candidate.clear();
                    reprocess = true;
                }
                continue;
            }

            if (isCsiFinal(byte)) {
                m_candidate.append(byte);
                ordinary.append(m_candidate);
                SynchronizedUpdateSegment observed;
                observeCsi(m_candidate, &observed);
                m_candidate.clear();
                flush(observed);
            } else if (isCsiBody(byte) && m_candidate.size() < kMaximumObservedCsiBytes) {
                m_candidate.append(byte);
            } else {
                // Malformed or pathologically long CSI: preserve it verbatim,
                // then reconsider this byte as a possible new ESC sequence.
                ordinary.append(m_candidate);
                m_candidate.clear();
                reprocess = true;
            }
        }
    }
    flush({});
    return result;
}

QByteArray SynchronizedUpdateParser::flushPending() {
    return std::exchange(m_candidate, {});
}

TerminalCoreWorker::TerminalCoreWorker(QString sessionId,
                                       int columns,
                                       int rows,
                                       int maxScrollback,
                                       std::shared_ptr<TerminalOutputQueue> outputQueue,
                                       std::shared_ptr<TerminalFrameStore> frameStore,
                                       QObject *parent, EngineFactory engineFactory)
    : QObject(parent),
      m_sessionId(std::move(sessionId)),
      m_columns(std::max(1, columns)),
      m_rows(std::max(1, rows)),
      m_maxScrollback(maxScrollback),
      m_outputQueue(std::move(outputQueue)),
      m_frameStore(std::move(frameStore)),
      m_engineFactory(std::move(engineFactory)) {
    m_clock.start();
}

TerminalCoreWorker::~TerminalCoreWorker() = default;

bool TerminalCoreWorker::ensureInitialized() {
    if (m_shuttingDown || m_initializationFailed) return false;
    if (!m_initialized) initialize();
    return m_initialized;
}

void TerminalCoreWorker::initialize() {
    if (m_initialized || m_initializationFailed || m_shuttingDown) return;
    try {
        createEngine();
    } catch (...) {
        m_initializationFailed = true;
        emit initializationFinished(false);
        return;
    }

    m_presentTimer = new QTimer(this);
    m_presentTimer->setSingleShot(true);
    m_presentTimer->setTimerType(Qt::PreciseTimer);
    connect(m_presentTimer, &QTimer::timeout, this, [this]() { schedulePresentation(true); });
    m_searchTimer = new QTimer(this);
    m_searchTimer->setSingleShot(true);
    connect(m_searchTimer, &QTimer::timeout, this, &TerminalCoreWorker::searchStep);
    m_autoscrollTimer = new QTimer(this);
    m_autoscrollTimer->setInterval(kAutoscrollIntervalMs);
    connect(m_autoscrollTimer, &QTimer::timeout, this, &TerminalCoreWorker::autoscrollTick);
    m_compressTimer = new QTimer(this);
    m_compressTimer->setSingleShot(true);
    m_compressTimer->setInterval(kCompressIdleMs);
    connect(m_compressTimer, &QTimer::timeout, this, [this]() {
        // A session nobody watches will not scroll back soon: compress all.
        if (m_engine) m_engine->compressHistory(!m_rendererActive);
    });
    m_overscanTimer = new QTimer(this);
    m_overscanTimer->setSingleShot(true);
    m_overscanTimer->setInterval(kOverscanSettleMs);
    connect(m_overscanTimer, &QTimer::timeout, this, [this]() {
        // The drag stopped: the next frame brings the overscan smooth
        // scrolling draws from.
        if (!m_engine) return;
        m_engine->setWideOverscan(true);
        requestPresentation(false);
    });
    m_scheduler.setFrameIntervalMs(frameIntervalMs());
    markDirty();
    m_initialized = true;
    emit initializationFinished(true);
}

void TerminalCoreWorker::createEngine() {
    std::unique_ptr<GhosttyEngine> engine =
        m_engineFactory ? m_engineFactory(m_columns, m_rows) : std::make_unique<GhosttyEngine>(m_columns, m_rows);
    if (!engine) throw std::runtime_error("Terminal engine factory returned null");
    if (!engine->isReady()) {
        throw std::runtime_error("Terminal engine failed to create its terminal");
    }
    engine->setMaxScrollback(m_maxScrollback);
    engine->setDefaultColors(m_defaultForeground, m_defaultBackground);
    engine->setAnsiPalette(m_ansiPalette);
    engine->setDefaultCursorShape(m_cursorShape);
    engine->setCellPixelSize(m_cellPixelSize);
    engine->setOutputCallback([this](const QByteArray &bytes) {
        if (!bytes.isEmpty()) emit outputRequested(bytes, m_inputToken);
    });
    engine->setTitleCallback([this](const QString &title) { emit titleChanged(title, m_inputToken); });
    engine->setClipboardCallback([this](const QString &text) { emit clipboardWriteRequested(text, m_inputToken); });
    engine->setHostEventCallback(
        [this](const TerminalHostEvent &event) { emit hostEventRequested(event, m_inputToken); });
    engine->setSearchNeedle(m_searchNeedle);
    m_engine = std::move(engine);
}

void TerminalCoreWorker::setDefaultColors(const QColor &foreground, const QColor &background) {
    m_defaultForeground = foreground;
    m_defaultBackground = background;
    if (!m_initialized || !m_engine) {
        return;
    }
    m_engine->setDefaultColors(m_defaultForeground, m_defaultBackground);
    requestPresentation(false);
}

void TerminalCoreWorker::setCellPixelSize(QSize size) {
    m_cellPixelSize = size;
    if (m_engine) m_engine->setCellPixelSize(m_cellPixelSize);
}

void TerminalCoreWorker::setAnsiPalette(const QVector<QColor> &palette) {
    if (palette.size() != 16) {
        return;
    }
    m_ansiPalette = palette;
    if (!m_initialized || !m_engine) {
        return;
    }
    m_engine->setAnsiPalette(m_ansiPalette);
    requestPresentation(false);
}

void TerminalCoreWorker::setDefaultCursorShape(CursorShape shape)
{
    m_cursorShape = shape;
    if (!m_initialized || !m_engine) return;
    m_engine->setDefaultCursorShape(shape);
    requestPresentation(true);
}

void TerminalCoreWorker::shutdown() {
    m_shuttingDown = true;
    m_process.reset();
    if (!m_initialized) return;
    m_presentTimer->stop();
    m_searchTimer->stop();
    m_autoscrollTimer->stop();
    m_compressTimer->stop();
    m_overscanTimer->stop();
    QByteArray bytes;
    while (m_outputQueue && m_outputQueue->take(&bytes, kWorkerOutputSliceBytes)) {
        processOutput(bytes);
    }
    const QByteArray pendingProtocol = m_protocolParser.flushPending();
    if (!pendingProtocol.isEmpty() && m_engine) m_engine->feed(pendingProtocol);
    m_engine.reset();
    m_initialized = false;
}

void TerminalCoreWorker::attachProcess(std::shared_ptr<TerminalProcess> process, int columns, int rows)
{
    m_process = std::move(process);
    if (m_process && (columns != m_columns || rows != m_rows)) {
        // The view moved on while the child started.
        m_scheduler.repaintExpected(nowMs());
        m_process->requestResize(m_columns, m_rows);
    }
}

void TerminalCoreWorker::finishTransport() {
    m_process.reset();
    if (!ensureInitialized()) {
        return;
    }

    // The producer has already stopped, so this reaches a stable queue tail
    // and also releases a wake reservation left by the final PTY callback.
    QByteArray bytes;
    while (m_outputQueue && m_outputQueue->take(&bytes, kWorkerOutputSliceBytes)) {
        processOutput(bytes);
    }
    if (m_outputQueue) {
        m_outputQueue->finishWake();
    }

    const QByteArray incompleteControl = m_protocolParser.flushPending();
    if (!incompleteControl.isEmpty()) {
        // Show the received bytes, then cancel the incomplete escape.
        m_engine->feed(incompleteControl);
        m_engine->feed(QByteArrayView("\x18", 1)); // CAN
    }

    // A dead transport can never complete a 2026 transaction or a reprint;
    // present its final state.
    m_scheduler.resetProtocol();
    requestPresentation(true);
}

void TerminalCoreWorker::beginTransportEpoch() {
    if (!ensureInitialized())
        return;

    m_presentTimer->stop();
    m_searchTimer->stop();
    m_autoscrollTimer->stop();
    m_compressTimer->stop();
    m_overscanTimer->stop();

    std::unique_ptr<GhosttyEngine> previous = std::move(m_engine);
    try {
        createEngine();
    } catch (...) {
        // A missing engine would look live and drop every keystroke.
        m_engine = std::move(previous);
        emit transportEpochFailed();
        return;
    }
    previous.reset();
    ++m_transportEpoch;
    m_protocolParser = {};
    m_pasteActive = false;
    m_scheduler.resetProtocol();
    ++m_liveFullRefreshRevision;
    ++m_viewportJump;
    markDirty();
    if (m_frameStore)
        m_frameStore->discardNotification();
    emit titleChanged(QString(), m_inputToken);
    // Before returning: the old child's snapshot would pass for a ready
    // prompt, and early input would land at its cursor.
    publishSnapshot();
    scheduleSearchStep();
}

void TerminalCoreWorker::drainOutput() {
    if (!ensureInitialized()) return;
    QElapsedTimer budget;
    budget.start();
    qsizetype processed = 0;
    QByteArray bytes;
    bool endsWrite = false;
    while (processed < kWorkerOutputBudgetBytes && budget.elapsed() < kWorkerOutputBudgetMs &&
           m_outputQueue && m_outputQueue->take(&bytes, kWorkerOutputSliceBytes, &endsWrite)) {
        processed += bytes.size();
        processOutput(bytes);
        // Only the end of a producer write is a cut point; evaluating per
        // chunk publishes there instead of wherever a slice or timer lands.
        m_scheduler.outputConsumed(nowMs(), endsWrite);
        schedulePresentation();
    }
    if (processed > 0) {
        scheduleSearchStep();
        m_compressTimer->start();
    }
    if (m_outputQueue && m_outputQueue->finishWake()) {
        QMetaObject::invokeMethod(this, &TerminalCoreWorker::drainOutput, Qt::QueuedConnection);
    }
}

void TerminalCoreWorker::processOutput(const QByteArray &bytes) {
    if (!m_engine || bytes.isEmpty()) return;
    m_receivedOutputBytes += quint64(bytes.size());
    for (const SynchronizedUpdateSegment &segment : m_protocolParser.push(bytes)) {
        if (!segment.bytes.isEmpty()) {
            m_engine->feed(segment.bytes);
            // Only records the change; presentation is decided at the next
            // cut point. Rows that did not change are reused by the frame.
            markDirty();
        }
        if (segment.win32InputAfter) m_engine->setWin32InputMode(*segment.win32InputAfter);
        if (segment.modifyOtherKeysAfter >= 0) m_engine->setModifyOtherKeys(segment.modifyOtherKeysAfter);
        handleBoundary(segment.boundaryAfter);
    }
    if (!m_activityClock.isValid() || m_activityClock.elapsed() >= kActivitySignalIntervalMs) {
        m_activityClock.restart();
        emit outputActivity(m_inputToken);
    }
}

void TerminalCoreWorker::handleBoundary(SynchronizedUpdateBoundary boundary) {
    switch (boundary) {
    case SynchronizedUpdateBoundary::None:
        return;
    case SynchronizedUpdateBoundary::RedrawBegin:
        // ED3 starts a whole-transcript reprint (omp/pi-tui, without 2026).
        m_scheduler.redrawBegin(nowMs());
        break;
    case SynchronizedUpdateBoundary::Begin:
        m_scheduler.synchronizedBegin(nowMs());
        break;
    case SynchronizedUpdateBoundary::End:
        // The application's frame is complete exactly here, even when the
        // producer's write continues with the next transaction; publish it
        // before those bytes are parsed.
        m_scheduler.synchronizedEnd(nowMs());
        schedulePresentation(true);
        return;
    }
    schedulePresentation();
}

int TerminalCoreWorker::frameIntervalMs() const {
    const int interactive = std::max(
        1, int(std::ceil(1000.0 / std::clamp(m_maximumFramesPerSecond, 30, 360))));
    if (!m_rendererActive) {
        return std::max(interactive, 100);
    }
    return interactive;
}

void TerminalCoreWorker::requestPresentation(bool immediate) {
    markDirty();
    if (immediate) m_scheduler.requestImmediate();
    schedulePresentation();
}

void TerminalCoreWorker::schedulePresentation(bool publishInline) {
    // Publishes without a consumer too (at a lower rate): a reattaching view
    // must find background output in latest().
    if (!m_initialized || m_shuttingDown || !m_presentTimer) return;
    const qint64 now = nowMs();
    const TerminalPresentationScheduler::Decision decision =
        m_scheduler.evaluate(now, m_outputQueue && m_outputQueue->size() > 0);
    if (decision.released == TerminalPresentationScheduler::Release::SynchronizedStall) {
        // ratatui apps wrap every frame in 2026; a lost end must not freeze
        // all later redraws.
        qCWarning(terminalProtocolLog) << "PTY synchronized update received no bytes for" << kSynchronizedStallMs
                                       << "ms before its reset; releasing presentation";
    }
    if (decision.publish && !publishInline) {
        // Writes already queued behind this one coalesce into the frame; the
        // timer re-evaluates, so a cut point lost meanwhile is waited for.
        m_presentTimer->start(0);
        return;
    }
    if (decision.publish) {
        m_pendingForcedCut = decision.forcedCut;
        if (decision.forcedCut) ++m_forcedCuts;
        m_presentTimer->stop();
        publishSnapshot();
        return;
    }
    if (decision.recheckAtMs < 0) {
        m_presentTimer->stop();
        return;
    }
    m_presentTimer->start(int(std::clamp<qint64>(decision.recheckAtMs - now, 0, 60'000)));
}

void TerminalCoreWorker::publishSnapshot() {
    if (!ensureInitialized() || !m_scheduler.dirty() || m_scheduler.deferred()) {
        return;
    }

    // The engine pairs the cursor, selection and search highlights with the
    // exact rows of one terminal state; only a protocol-declared CSI ?2026
    // transaction may defer that state.
    TerminalFrameSnapshot snapshot;
    snapshot.frame = m_engine->frame();
    snapshot.search = m_engine->searchStatus();
    snapshot.graphicsRevision = m_engine->graphicsRevision();
    snapshot.transportEpoch = m_transportEpoch;
    snapshot.viewportEpoch = m_viewportEpoch;
    snapshot.scrollRequest = m_scrollRequest;
    snapshot.viewportJump = m_viewportJump;
    const quint64 fullRevision = m_liveFullRefreshRevision;
    snapshot.sequence = ++m_sequence;
    snapshot.mouseMode = m_engine->mouseTracking() ? 1 : 0;
    snapshot.synchronizedUpdateActive = m_scheduler.synchronizedActive();
    snapshot.fullRefresh = fullRevision != m_lastPublishedFullRefreshRevision;

    QJsonObject traceDetails;
    const bool traceFrame = m_viewportTraceFrames > 0 && TerminalViewportTrace::enabled();
    if (traceFrame) {
        --m_viewportTraceFrames;
        traceDetails = TerminalViewportTrace::describeFrame(snapshot.frame);
        traceDetails.insert(QStringLiteral("epoch"), qint64(snapshot.viewportEpoch));
        traceDetails.insert(QStringLiteral("sequence"), qint64(snapshot.sequence));
        traceDetails.insert(QStringLiteral("fullRefresh"), snapshot.fullRefresh);
        traceDetails.insert(QStringLiteral("historyLines"), m_engine->scrollbackRows());
        traceDetails.insert(QStringLiteral("outputBytes"), qint64(m_receivedOutputBytes));
        traceDetails.insert(QStringLiteral("queuedBytes"), m_outputQueue ? qint64(m_outputQueue->size()) : 0);
        traceDetails.insert(QStringLiteral("forcedCut"), m_pendingForcedCut);
        traceDetails.insert(QStringLiteral("forcedCuts"), qint64(m_forcedCuts));
    }
    const bool needsNotification = m_frameStore->publish(std::move(snapshot));
    if (traceFrame) {
        traceDetails.insert(QStringLiteral("notify"), needsNotification);
        traceDetails.insert(QStringLiteral("rendererActive"), m_rendererActive);
        TerminalViewportTrace::record(m_sessionId, QStringLiteral("core-frame"), traceDetails);
    }
    m_lastPublishedFullRefreshRevision = fullRevision;
    m_scheduler.published(nowMs());
    m_pendingForcedCut = false;
    if (!needsNotification) {
        return;
    }
    if (m_rendererActive) {
        emit frameAvailable();
    } else {
        // Release the reservation so the next attach gets notified.
        if (m_frameStore) {
            m_frameStore->discardNotification();
        }
    }
}

void TerminalCoreWorker::setRenderConsumer(bool active, int maximumFramesPerSecond) {
    if (active && !ensureInitialized()) return;
    const bool wasActive = m_rendererActive;
    const int previousRate = m_maximumFramesPerSecond;
    m_rendererActive = active;
    m_maximumFramesPerSecond = std::clamp(maximumFramesPerSecond, 30, 360);
    m_scheduler.setFrameIntervalMs(frameIntervalMs());
    TerminalViewportTrace::record(m_sessionId, QStringLiteral("core-consumer"),
        {{QStringLiteral("active"), active}, {QStringLiteral("wasActive"), wasActive},
         {QStringLiteral("snapshotDirty"), m_scheduler.dirty()},
         {QStringLiteral("epoch"), qint64(m_viewportEpoch)},
         {QStringLiteral("sequence"), qint64(m_sequence)}});
    if (!active) {
        if (m_frameStore) m_frameStore->discardNotification();
        schedulePresentation();
        return;
    }
    if (!wasActive) {
        // A new consumer has no prepared rows to reuse.
        ++m_liveFullRefreshRevision;
        requestPresentation(true);
        return;
    }
    if (previousRate != m_maximumFramesPerSecond) schedulePresentation();
}

void TerminalCoreWorker::resize(int columns, int rows, quint64 viewportEpoch) {
    if (!ensureInitialized()) return;
    m_viewportTraceFrames = 8;
    TerminalViewportTrace::record(m_sessionId, QStringLiteral("core-resize"),
        {{QStringLiteral("oldColumns"), m_columns}, {QStringLiteral("oldRows"), m_rows},
         {QStringLiteral("columns"), columns}, {QStringLiteral("rows"), rows},
         {QStringLiteral("epoch"), qint64(viewportEpoch)},
         {QStringLiteral("outputBytes"), qint64(m_receivedOutputBytes)}});
    columns = std::max(1, columns);
    rows = std::max(1, rows);
    const bool viewportEpochChanged = m_viewportEpoch != viewportEpoch;
    m_viewportEpoch = viewportEpoch;
    if (columns == m_columns && rows == m_rows) {
        if (viewportEpochChanged) {
            ++m_liveFullRefreshRevision;
            requestPresentation(true);
        }
        return;
    }
    m_columns = columns;
    m_rows = rows;
    if (TerminalViewportTrace::enabled()) {
        auto details = TerminalViewportTrace::describeFrame(m_engine->frame());
        details.insert(QStringLiteral("epoch"), qint64(viewportEpoch));
        details.insert(QStringLiteral("historyLines"), m_engine->scrollbackRows());
        TerminalViewportTrace::record(m_sessionId, QStringLiteral("core-before-resize"), details);
    }
    // Reflow moves the viewport; the view takes the new position.
    m_engine->resize(columns, rows);
    ++m_liveFullRefreshRevision;
    ++m_viewportJump;
    requestPresentation(true);
    m_scheduler.repaintExpected(nowMs());
    if (m_process) m_process->requestResize(columns, rows);
}

void TerminalCoreWorker::clearDisplay() {
    if (!ensureInitialized()) return;
    m_engine->clearDisplay();
    ++m_viewportJump;
    requestPresentation(true);
    scheduleSearchStep();
    emit displayCleared();
}

void TerminalCoreWorker::setInputEpoch(std::shared_ptr<TerminalInputEpoch> epoch) {
    m_inputToken = {std::move(epoch), 0, TerminalInputKind::Protocol};
}

void TerminalCoreWorker::executeInput(
    const TerminalInputToken &token,
    const std::function<void(TerminalCoreWorker *)> &operation) {
    if (!token.valid()) return;
    const TerminalInputToken previous = m_inputToken;
    m_inputToken = token;
    operation(this);
    m_inputToken = previous;
}

void TerminalCoreWorker::followInput()
{
    if (m_engine->scrollbar().atBottom()) return;
    m_engine->scrollViewportToBottom();
    ++m_viewportJump;
    requestPresentation(true);
}

void TerminalCoreWorker::sendKeyStroke(const TerminalKeyStroke &stroke) {
    if (!ensureInitialized()) return;
    if (stroke.event != TerminalKeyEvent::Release && (stroke.key || !stroke.text.isEmpty())) {
        followInput();
    }
    m_engine->sendKeyStroke(stroke);
}
void TerminalCoreWorker::sendText(const QString &text) {
    if (ensureInitialized() && !text.isEmpty()) {
        followInput();
        m_engine->sendText(text);
    }
}
void TerminalCoreWorker::beginPaste() {
    if (ensureInitialized() && !m_pasteActive) {
        m_pasteActive = true;
        followInput();
        m_engine->beginPaste();
    }
}
void TerminalCoreWorker::endPaste() {
    if (ensureInitialized() && m_pasteActive) {
        m_pasteActive = false;
        m_engine->endPaste();
    }
}
void TerminalCoreWorker::sendPasteBody(const QString &text) {
    if (ensureInitialized() && !text.isEmpty()) {
        m_engine->sendPasteBody(text);
    }
}
void TerminalCoreWorker::sendMouseMove(int row, int column, int modifiers) {
    if (ensureInitialized()) {
        m_engine->sendMouseMove(row, column, Qt::KeyboardModifiers(modifiers));
    }
}
void TerminalCoreWorker::sendMouseButton(int button, bool pressed, int row, int column, int modifiers) {
    if (ensureInitialized() && button >= int(TerminalMouseButton::Left) &&
        button <= int(TerminalMouseButton::WheelRight)) {
        m_engine->sendMouseButton(TerminalMouseButton(button), pressed, row, column,
                                  Qt::KeyboardModifiers(modifiers));
    }
}
void TerminalCoreWorker::setFocused(bool focused) {
    if (ensureInitialized()) m_engine->setFocused(focused);
}

void TerminalCoreWorker::scrollViewport(qint64 row, quint64 request)
{
    if (!ensureInitialized()) return;
    const qint64 previous = m_engine->scrollbar().offset;
    m_engine->scrollViewportTo(row);
    adaptOverscan(std::abs(row - previous) > m_rows);
    m_scrollRequest = std::max(m_scrollRequest, request);
    requestPresentation(true);
}

void TerminalCoreWorker::adaptOverscan(bool jumped)
{
    if (jumped) {
        m_engine->setWideOverscan(false);
        m_overscanTimer->start();
    } else {
        m_overscanTimer->stop();
        m_engine->setWideOverscan(true);
    }
}

void TerminalCoreWorker::scrollViewportToBottom(quint64 request)
{
    if (!ensureInitialized()) return;
    adaptOverscan(false);
    m_engine->scrollViewportToBottom();
    m_scrollRequest = std::max(m_scrollRequest, request);
    requestPresentation(true);
}

void TerminalCoreWorker::scrollToPrompt(int delta)
{
    if (!ensureInitialized() || delta == 0) return;
    m_engine->scrollViewportByPrompts(delta);
    ++m_viewportJump;
    requestPresentation(true);
}

void TerminalCoreWorker::selectionEvent(const TerminalSelectionEvent &event)
{
    if (!ensureInitialized()) return;
    const bool autoscroll = m_engine->selectionEvent(event);
    if (autoscroll && !m_autoscrollTimer->isActive()) m_autoscrollTimer->start();
    if (!autoscroll) m_autoscrollTimer->stop();
    requestPresentation(true);
}

void TerminalCoreWorker::autoscrollTick()
{
    if (!m_engine || !m_engine->selectionAutoscrollTick()) {
        m_autoscrollTimer->stop();
        return;
    }
    ++m_viewportJump;
    requestPresentation(true);
}

void TerminalCoreWorker::selectAll()
{
    if (!ensureInitialized()) return;
    m_engine->selectAll();
    requestPresentation(true);
}

void TerminalCoreWorker::clearSelection()
{
    if (!ensureInitialized()) return;
    m_autoscrollTimer->stop();
    m_engine->clearSelection();
    requestPresentation(true);
}

QString TerminalCoreWorker::selectedText() const
{
    return m_initialized && m_engine ? m_engine->selectedText() : QString();
}

void TerminalCoreWorker::setSearchNeedle(const QString &needle)
{
    m_searchNeedle = needle;
    // A new needle scrolls to its newest match once the first one is found.
    m_searchRevealPending = !needle.isEmpty();
    if (!ensureInitialized()) return;
    m_engine->setSearchNeedle(needle);
    requestPresentation(true);
    scheduleSearchStep();
}

void TerminalCoreWorker::selectSearchMatch(bool newer)
{
    if (!ensureInitialized() || m_searchNeedle.isEmpty()) return;
    // Matches beyond what has been counted so far are not selectable yet.
    m_searchRevealPending = false;
    m_engine->advanceSearch();
    m_engine->selectSearchMatch(newer);
    ++m_viewportJump;
    requestPresentation(true);
    scheduleSearchStep();
}

void TerminalCoreWorker::scheduleSearchStep()
{
    if (m_searchNeedle.isEmpty() || !m_searchTimer || m_searchTimer->isActive()) return;
    // A zero timer lets queued output and input run between steps.
    m_searchTimer->start(0);
}

void TerminalCoreWorker::searchStep()
{
    if (!m_engine || m_searchNeedle.isEmpty()) return;
    const bool more = m_engine->advanceSearch();
    if (m_searchRevealPending) {
        const TerminalSearchStatus status = m_engine->searchStatus();
        if (status.total > 0) {
            // With nothing selected, the older-ward step selects the newest match.
            m_searchRevealPending = false;
            m_engine->selectSearchMatch(false);
            ++m_viewportJump;
        } else if (!more) {
            m_searchRevealPending = false;
        }
    }
    requestPresentation(false);
    if (more) m_searchTimer->start(0);
}

} // namespace span::terminal
