#include "terminal/render/terminal_view.h"

#include "terminal/core/terminal_color.h"
#include "terminal/core/terminal_viewport_trace.h"
#include "terminal/render/terminal_cursor_animation.h"
#include "terminal/render/terminal_ime_anchor_resolver.h"
#include "terminal/render/terminal_scene_graph.h"

#include "terminal/api/terminal_session.h"
#include "terminal/terminal_constants.h"

#include <QSGNode>
#include <QQuickWindow>

#include <algorithm>
#include <cmath>
#include <limits>

// Input handling (keys, mouse, wheel, IME, links) is in terminal_view_input.cpp.

namespace span::terminal {

namespace {

// Face size until the settings apply one.
constexpr int kDefaultFontSize = 13;
constexpr int kFrameRateSampleMs = 500;
// SGR 5 blink phase.
constexpr int kTextBlinkMs = 530;
// Content at or above this rate is busy: the caret stops fading.
constexpr int kBusyContentFps = 15;
constexpr int kAnsiPaletteSize = 16;
// Frames and paints traced after a session binding or grid change.
constexpr int kViewportTraceBudget = 8;

enum class SnapshotDisposition { Ignore, AwaitMatching, Apply };

struct SnapshotDecision {
    SnapshotDisposition disposition = SnapshotDisposition::Ignore;
    bool rebuildAll = false;
};

// Whether a prepared snapshot replaces the presented frame. While the view
// expects a viewport generation, only a snapshot of it is applied; others
// are acknowledged and the last frame stays.
SnapshotDecision acceptSnapshot(const TerminalFrameSnapshot &snapshot, quint64 lastSequence, quint64 transportEpoch,
                                quint64 viewportEpoch, const Frame &presented)
{
    SnapshotDecision decision;
    if (snapshot.sequence == 0 || snapshot.sequence == lastSequence) {
        return decision;
    }
    if (lastSequence != 0 && snapshot.sequence < lastSequence && snapshot.transportEpoch == transportEpoch) {
        // Never regress to an older frame within the same transport epoch
        // (e.g. a rebuilt frame store restarting its sequence counter).
        return decision;
    }
    if (viewportEpoch != 0 && snapshot.viewportEpoch != viewportEpoch) {
        decision.disposition = SnapshotDisposition::AwaitMatching;
        return decision;
    }
    decision.disposition = SnapshotDisposition::Apply;
    decision.rebuildAll =
        snapshot.fullRefresh || snapshot.frame.columns != presented.columns || snapshot.frame.rows != presented.rows;
    return decision;
}

} // namespace

TerminalView::TerminalView(QQuickItem *parent) : QQuickItem(parent) {
    setFlag(ItemHasContents, true);
    setFlag(ItemAcceptsInputMethod, true);
    setActiveFocusOnTab(true);
    setAcceptedMouseButtons(Qt::AllButtons);
    setAcceptHoverEvents(true);
    setCursor(Qt::IBeamCursor);

    m_font = TerminalFontSet::safeFixedFont(kDefaultFontSize);
    m_fontSet.setFont(m_font);
    m_ansiPalette = defaultAnsiPalette(false);
    m_preparer = std::make_unique<TerminalFramePreparer>();
    connect(m_preparer.get(), &TerminalFramePreparer::prepared, this, &TerminalView::scheduleFrameUpdate,
            Qt::QueuedConnection);
    publishRenderStyle();

    m_frameRateTimer.setInterval(kFrameRateSampleMs);
    m_frameRateTimer.setTimerType(Qt::CoarseTimer);
    connect(&m_frameRateTimer, &QTimer::timeout,
            this, &TerminalView::sampleContentFrameRate);
    m_frameRateClock.start();
    m_scrollClock.start();
    m_textBlinkTimer.setInterval(kTextBlinkMs);
    connect(&m_textBlinkTimer, &QTimer::timeout, this, [this]() {
        m_textBlinkLit = !m_textBlinkLit;
        if (hasBlinkingText()) {
            update();
        }
    });
    m_cursorAnimationTimer.setTimerType(Qt::PreciseTimer);
    connect(&m_cursorAnimationTimer, &QTimer::timeout, this, [this]() {
        if (m_cursorAnimationEnabled) {
            if (!m_cursorAnimationClock.isValid()) {
                m_cursorAnimationClock.start();
            }
            m_cursorOpacity = TerminalCursorAnimation::opacityAt(
                m_cursorAnimationClock.elapsed());
        } else {
            m_cursorDiscreteLit = !m_cursorDiscreteLit;
            m_cursorOpacity = m_cursorDiscreteLit ? 1.0 : 0.0;
        }
        update();
    });
    m_cursorHoldTimer.setSingleShot(true);
    m_cursorHoldTimer.setTimerType(Qt::CoarseTimer);
    m_cursorHoldTimer.setInterval(kCursorBlinkHoldMs);
    connect(&m_cursorHoldTimer, &QTimer::timeout, this, [this]() {
        updateCursorAnimation();
    });
    connect(this, &QQuickItem::activeFocusChanged, this, [this]() {
        resetCursorAnimation();
        if (hasActiveFocus()) {
            noteCursorHoldRestart();
        } else {
            updateCursorAnimation();
        }
        updateInputMethodQueries();
        update();
    });
    connect(this, &QQuickItem::visibleChanged, this, [this]() {
        resetCursorAnimation();
        updateCursorAnimation();
        syncRenderConsumer();
        syncFrameRateSampling();
        update();
    });
    connect(this, &QQuickItem::windowChanged, this, [this](QQuickWindow *window) {
        m_windowBinding.reset();
        const quint64 generation = m_windowBinding.generation;
        const auto isCurrentWindow = [this, window, generation]() {
            return m_windowBinding.generation == generation && this->window() == window;
        };
        if (window) {
            m_windowBinding.connections.push_back(connect(
                window, &QQuickWindow::afterRendering, this,
                [counter = m_contentFrames]() {
                    if (counter->pending.exchange(false, std::memory_order_acq_rel))
                        counter->frames.fetch_add(1, std::memory_order_relaxed);
                }, Qt::DirectConnection));
            // Emitted on the GUI thread once per frame after polishing and
            // before the sync, so a scroll step taken here is drawn in the
            // same frame.
            m_windowBinding.connections.push_back(
                connect(window, &QQuickWindow::afterAnimating, this, [this, isCurrentWindow]() {
                    if (isCurrentWindow()) advanceScrollAnimation();
                }));
            m_windowBinding.connections.push_back(connect(
                window, &QQuickWindow::activeChanged, this, [this, isCurrentWindow]() {
                    if (!isCurrentWindow()) return;
                    resetCursorAnimation();
                    if (this->window()->isActive() && hasActiveFocus()) {
                        noteCursorHoldRestart();
                    } else {
                        updateCursorAnimation();
                    }
                    update();
                }));
            m_windowBinding.connections.push_back(
                connect(window, &QQuickWindow::visibilityChanged, this, [this, isCurrentWindow]() {
                    if (!isCurrentWindow()) return;
                    syncRenderConsumer();
                    syncFrameRateSampling();
                }));
            m_windowBinding.connections.push_back(connect(
                window, &QQuickWindow::screenChanged, this, [this, isCurrentWindow]() {
                    QTimer::singleShot(0, this, [this, isCurrentWindow]() {
                        if (isCurrentWindow()) commitViewport();
                    });
                }));
        }
        updateCursorAnimation();
        syncRenderConsumer();
        syncFrameRateSampling();
    });
}

TerminalView::~TerminalView() {
    // ~QQuickItem still emits windowChanged/visibleChanged/activeFocusChanged
    // after these members are gone; disconnect first, the session's too.
    disconnect(this, nullptr, this, nullptr);
    if (m_session)
        disconnect(m_session, nullptr, this, nullptr);
    // Waits for this view's frame in flight before anything it reads goes away.
    m_preparer.reset();
    m_windowBinding.reset();
    traceViewport(QStringLiteral("view-destroy"),
        {{QStringLiteral("consumer"), m_renderConsumerActive}});
    if (m_session) {
        m_session->detachRenderConsumer(m_renderConsumerToken);
    }
}

void TerminalView::traceViewport(const QString &stage, QJsonObject details) const
{
    if (!TerminalViewportTrace::enabled()) return;
    details.insert(QStringLiteral("view"), QString::number(quintptr(this), 16));
    TerminalViewportTrace::record(m_viewportTraceSession, stage, std::move(details));
}

TerminalSession *TerminalView::session() const { return m_session; }

void TerminalView::setSession(TerminalSession *session) {
    if (m_session == session
        && (!session || session->ownsRenderConsumer(m_renderConsumerToken))) {
        return;
    }
    traceViewport(QStringLiteral("view-bind"),
        {{QStringLiteral("next"), session ? session->id() : QString()},
         {QStringLiteral("consumer"), m_renderConsumerActive},
         {QStringLiteral("visible"), isVisible()},
         {QStringLiteral("width"), width()}, {QStringLiteral("height"), height()}});
    if (m_session) {
        clearImeComposition(true);
        m_session->detachRenderConsumer(m_renderConsumerToken);
        disconnect(m_session, nullptr, this, nullptr);
    }
    m_renderConsumerActive = false;
    m_renderConsumerToken = 0;
    m_altGrActive = false;
    m_forwardedKeys.clear();
    m_pressedLink.clear();
    m_session = session;
    m_viewportTraceSession = session ? session->id() : QString();
    m_viewportTraceFrames = kViewportTraceBudget;
    m_viewportTracePaints = kViewportTraceBudget;
    const quint64 bindingGeneration = ++m_sessionBindingGeneration;
    resetContentFrameRate();
    resetCursorAnimation();
    if (m_session) {
        m_renderConsumerToken = m_session->attachRenderConsumer(this);
        TerminalSession *watchedSession = m_session;
        const auto isCurrentBinding = [this, watchedSession, bindingGeneration]() {
            return m_session == watchedSession
                && m_sessionBindingGeneration == bindingGeneration;
        };
        connect(m_session, &TerminalSession::frameReady,
                this, [this, isCurrentBinding](quint64 sequence) {
                    if (isCurrentBinding()) {
                        handleFrameReady(sequence);
                    }
                });
        connect(m_session, &TerminalSession::transportEpochChanged,
                this, [this, isCurrentBinding]() {
                    if (isCurrentBinding()) {
                        handleTransportEpochChanged();
                    }
                });
        connect(m_session, &TerminalSession::bellRang, this, [this, isCurrentBinding]() {
            // BEL flashes the taskbar entry while the window is in the
            // background; in front, the child's own output is the signal.
            if (isCurrentBinding() && window() && !window()->isActive()) window()->alert(0);
        });
        connect(m_session, &TerminalSession::stateChanged, this,
                [this, isCurrentBinding]() {
            if (!isCurrentBinding()) {
                return;
            }
            if (m_session && !m_session->running()) {
                clearImeComposition(true);
            }
            updateInputMethodQueries(Qt::ImEnabled);
            updateCursorAnimation();
            scheduleFrameUpdate();
        });
        connect(m_session, &TerminalSession::renderConsumerChanged, this,
                [this, isCurrentBinding]() {
            if (!isCurrentBinding()) return;
            if (!m_session->ownsRenderConsumer(m_renderConsumerToken)) {
                clearImeComposition(true);
            }
            updateInputMethodQueries(Qt::ImEnabled);
            syncRenderConsumer();
            updateCursorAnimation();
        });
        connect(m_session, &QObject::destroyed, this,
                [this, watchedSession, bindingGeneration]() {
            if (m_session != watchedSession
                || m_sessionBindingGeneration != bindingGeneration) {
                return;
            }
            // Not setSession(nullptr): destroyed fires after the
            // TerminalSession members are gone.
            m_session = nullptr;
            ++m_sessionBindingGeneration;
            ++m_sceneGeneration;
            m_renderConsumerActive = false;
            m_renderConsumerToken = 0;
            m_scroll.reset();
            m_requestedRow = kNoRow;
            m_scrollRequest = 0;
            m_viewportJump = 0;
            m_selecting = false;
            m_wheelStepRemainder = 0;
            m_protocolWheelStepRemainder = 0;
            m_protocolWheelColumnRemainder = 0;
            m_lastSnapshotSequence = 0;
            m_renderTransportEpoch = 0;
            m_imeComposition.clear();
            m_presented.reset();
            m_submittedSnapshot.reset();
            m_preparer->reset();
            m_graphicsRevision = 0;
            invalidateAll();
            updateInputMethodQueries(Qt::ImEnabled);
            updateCursorAnimation();
            emit selectionChanged();
            emit searchChanged();
            emit scrollChanged();
            emit sessionChanged();
        });
        m_session->setDefaultCursorShape(defaultCursorShape());
    }
    // Attach as a consumer before pulling/requesting frames so reattach
    // publishes notify this view instead of being discarded as background.
    syncRenderConsumer();
    resetViewportForLiveSession();
    if (m_session) {
        // Hit-testing and selection may run before the first prepared frame
        // arrives: present the cells now and let the prepared rows follow.
        auto snapshot = m_session->latestSnapshotShared();
        if (snapshot && !snapshot->frame.lines.isEmpty()) {
            auto cellsOnly = std::make_shared<TerminalPreparedFrame>();
            cellsOnly->snapshot = std::move(snapshot);
            m_presented = std::move(cellsOnly);
            syncScroll(nullptr);
        }
    }
    updateCursorAnimation();
    if (m_session) {
        QTimer::singleShot(0, this, [this, bindingGeneration]() {
            if (m_sessionBindingGeneration == bindingGeneration
                && isVisible() && window() && window()->isActive() && m_session
                && m_session->ownsRenderConsumer(m_renderConsumerToken)) {
                forceActiveFocus();
            }
        });
    }
    updateInputMethodQueries(Qt::ImEnabled);
    if (ownsRenderConsumer() && !m_searchNeedle.isEmpty())
        m_session->setSearchNeedle(m_renderConsumerToken, m_searchNeedle);
    emit selectionChanged();
    emit searchChanged();
    emit sessionChanged();
}

QFont TerminalView::font() const { return m_font; }

void TerminalView::setFont(const QFont &font) {
    if (m_font == font) {
        return;
    }
    m_font = font;
    m_fontSet.setFont(m_font);
    refreshViewportMetrics();
    commitViewport();
    invalidateAll();
    updateInputMethodQueries();
    emit fontChanged();
    emit metricsChanged();
}

QColor TerminalView::foregroundColor() const { return m_foreground; }
void TerminalView::setForegroundColor(const QColor &color) {
    if (m_foreground == color) return;
    m_foreground = color;
    invalidateAll();
    emit colorsChanged();
}

QColor TerminalView::backgroundColor() const { return m_background; }
void TerminalView::setBackgroundColor(const QColor &color) {
    if (m_background == color) return;
    m_background = color;
    invalidateAll();
    emit colorsChanged();
}

QColor TerminalView::cursorColor() const { return m_cursor; }
void TerminalView::setCursorColor(const QColor &color) {
    if (m_cursor == color) return;
    m_cursor = color;
    update();
    emit colorsChanged();
}

QColor TerminalView::selectionColor() const { return m_selection; }
void TerminalView::setSelectionColor(const QColor &color) {
    if (m_selection == color) return;
    m_selection = color;
    update();
    emit colorsChanged();
}

QVariantList TerminalView::ansiPalette() const {
    QVariantList result;
    result.reserve(m_ansiPalette.size());
    for (const QColor &color : m_ansiPalette) {
        result.append(color);
    }
    return result;
}

void TerminalView::setAnsiPalette(const QVariantList &palette) {
    if (palette.size() != kAnsiPaletteSize) {
        return;
    }

    QVector<QColor> colors;
    colors.reserve(palette.size());
    for (const QVariant &entry : palette) {
        const QColor color = entry.value<QColor>();
        if (!color.isValid()) {
            return;
        }
        colors.append(color);
    }
    if (m_ansiPalette == colors) {
        return;
    }
    m_ansiPalette = std::move(colors);
    invalidateAll();
    emit colorsChanged();
}

bool TerminalView::unreadOutput() const { return m_unreadOutput; }
bool TerminalView::hasSelection() const {
    return frame().hasSelection;
}
qreal TerminalView::scrollOffset() const
{
    return m_scroll.offset();
}
int TerminalView::scrollbackLines() const
{
    return int(std::min<qint64>(m_scroll.liveTop(), std::numeric_limits<int>::max()));
}

void TerminalView::setSmoothScrolling(bool enabled)
{
    if (m_smoothScrolling == enabled) return;
    m_smoothScrolling = enabled;
    const bool moved = m_scroll.animating() && !enabled;
    m_scroll.setAnimated(enabled);
    if (moved) handleScrollMoved();
    emit smoothScrollingChanged();
}
qreal TerminalView::cellWidth() const { return m_fontSet.cellWidth(); }
qreal TerminalView::cellHeight() const { return m_fontSet.cellHeight(); }

int TerminalView::presentationState() const
{
    return int(m_viewportLifecycle.state());
}

void TerminalView::setPresentationState(int state)
{
    const int normalized = std::clamp(state, int(TerminalViewportLifecycle::State::Normal),
                                      int(TerminalViewportLifecycle::State::Closing));
    if (presentationState() == normalized) {
        return;
    }
    traceViewport(QStringLiteral("view-presentation"),
        {{QStringLiteral("from"), presentationState()}, {QStringLiteral("to"), normalized},
         {QStringLiteral("presentedSequence"), qint64(m_lastSnapshotSequence)}});
    m_viewportLifecycle.setState(static_cast<TerminalViewportLifecycle::State>(normalized));
    syncRenderConsumer();
    if (m_viewportLifecycle.state() == TerminalViewportLifecycle::State::Normal) {
        commitViewport();
        const auto snapshot = m_session ? m_session->latestSnapshotShared() : nullptr;
        if (snapshot && snapshot->sequence > m_lastSnapshotSequence) {
            scheduleFrameUpdate();
        }
    }
    emit presentationStateChanged();
}

void TerminalView::refreshViewportMetrics()
{
    m_fontSet.setFont(m_font);
    m_fontSet.setDevicePixelRatio(window() ? window()->devicePixelRatio() : 1.0);
    m_viewportLifecycle.setCellSize(QSizeF(m_fontSet.cellWidth(), m_fontSet.cellHeight()));
}

void TerminalView::commitViewport()
{
    refreshViewportMetrics();
    m_viewportLifecycle.setContentSize(QSizeF(width(), height()));
    // Only the presenting view sizes the session; a hidden one keeps its
    // commit pending until it becomes the active consumer.
    if (!m_renderConsumerActive) {
        return;
    }
    const TerminalViewportLifecycle::Commit commit = m_viewportLifecycle.takeStableCommit();
    if (!commit) {
        return;
    }
    // Inline images are drawn in logical pixels, so XTWINOPS reports those.
    m_session->setCellPixelSize(QSize(qRound(cellWidth()), qRound(cellHeight())));
    if (commit.gridChanged) {
        m_viewportTraceFrames = kViewportTraceBudget;
        m_viewportTracePaints = kViewportTraceBudget;
        m_viewportEpoch = m_session->commitViewport(commit.columns, commit.rows);
        traceViewport(QStringLiteral("view-commit"),
                      {{QStringLiteral("width"), width()},
                       {QStringLiteral("height"), height()},
                       {QStringLiteral("cellWidth"), cellWidth()},
                       {QStringLiteral("cellHeight"), cellHeight()},
                       {QStringLiteral("columns"), commit.columns},
                       {QStringLiteral("rows"), commit.rows},
                       {QStringLiteral("epoch"), qint64(m_viewportEpoch)}});
    }
    invalidateAll();
}

QString TerminalView::cursorStyle() const { return m_cursorStyle; }

void TerminalView::setCursorStyle(const QString &style) {
    const QString normalized =
        style == kCursorStyleBar || style == kCursorStyleUnderline ? style : QString(kCursorStyleBlock);
    if (m_cursorStyle == normalized) {
        return;
    }
    m_cursorStyle = normalized;
    // The engine draws the user's shape until the child asks for another.
    if (m_session) m_session->setDefaultCursorShape(defaultCursorShape());
    emit cursorStyleChanged();
}

CursorShape TerminalView::defaultCursorShape() const
{
    return m_cursorStyle == kCursorStyleUnderline ? CursorShape::Underline
        : m_cursorStyle == kCursorStyleBar        ? CursorShape::Bar
                                                  : CursorShape::Block;
}

int TerminalView::maximumFrameRate() const { return m_maximumFrameRate; }

void TerminalView::setMaximumFrameRate(int framesPerSecond) {
    const int normalized = std::clamp(framesPerSecond, kMinimumFrameRate, kMaximumFrameRate);
    if (m_maximumFrameRate == normalized) {
        return;
    }
    m_maximumFrameRate = normalized;
    if (m_session && m_renderConsumerActive) {
        m_session->setRenderConsumer(m_renderConsumerToken, true, m_maximumFrameRate);
    }
    emit maximumFrameRateChanged();
}

bool TerminalView::cursorAnimationEnabled() const {
    return m_cursorAnimationEnabled;
}

void TerminalView::setCursorAnimationEnabled(bool enabled) {
    if (m_cursorAnimationEnabled == enabled) {
        return;
    }
    m_cursorAnimationEnabled = enabled;
    resetCursorAnimation();
    updateCursorAnimation();
    emit cursorAnimationEnabledChanged();
}

void TerminalView::setSearchMatchColor(const QColor &color) {
    if (m_searchMatchColor == color) {
        return;
    }
    m_searchMatchColor = color;
    emit colorsChanged();
    update();
}

void TerminalView::setSearchCurrentColor(const QColor &color) {
    if (m_searchCurrentColor == color) {
        return;
    }
    m_searchCurrentColor = color;
    emit colorsChanged();
    update();
}

void TerminalView::setSearch(const QString &query)
{
    if (query == m_searchNeedle) {
        return;
    }
    m_searchNeedle = query;
    // Status and highlights arrive with the following frames.
    if (ownsRenderConsumer()) m_session->setSearchNeedle(m_renderConsumerToken, query);
    emit searchChanged();
}

void TerminalView::clearSearch() {
    setSearch(QString());
}

void TerminalView::findNext(bool backwards) {
    if (searchActive() && ownsRenderConsumer()) m_session->selectSearchMatch(m_renderConsumerToken, !backwards);
}

const TerminalSearchStatus *TerminalView::searchStatus() const
{
    const TerminalFrameSnapshot *snapshot = m_presented ? m_presented->snapshot.get() : nullptr;
    return snapshot && searchActive() && snapshot->search.needle == m_searchNeedle ? &snapshot->search : nullptr;
}

int TerminalView::searchMatchCount() const
{
    const TerminalSearchStatus *status = searchStatus();
    return status ? status->total : 0;
}

int TerminalView::searchCurrentIndex() const
{
    const TerminalSearchStatus *status = searchStatus();
    return status ? status->selected + 1 : 0;
}

bool TerminalView::searchPending() const
{
    const TerminalSearchStatus *status = searchStatus();
    return searchActive() && (!status || !status->complete);
}

void TerminalView::scrollToBottom() {
    if (m_scroll.scrollToBottom() || m_unreadOutput) {
        handleScrollMoved();
    }
}

void TerminalView::setScrollOffset(qreal offset)
{
    if (m_scroll.scrollTo(double(m_scroll.liveTop()) - offset)) {
        handleScrollMoved();
    }
}

void TerminalView::handleScrollMoved()
{
    if (!m_scroll.atBottom()) {
        clearImeComposition(true);
    } else if (m_unreadOutput) {
        m_unreadOutput = false;
        emit unreadOutputChanged();
    }
    requestCoreScroll();
    emit scrollChanged();
    // The drawn rows follow the scroll position at the next sync, from the
    // overscan of the presented frame until the core's next frame arrives.
    update();
    updateCursorAnimation();
    updateInputMethodQueries();
}

void TerminalView::requestCoreScroll()
{
    if (!ownsRenderConsumer()) return;
    // Whole rows: the fraction is drawn from the overscan.
    const qint64 row = m_scroll.atBottom() ? kBottomRow : qint64(std::floor(m_scroll.top()));
    if (row == m_requestedRow) return;
    m_requestedRow = row;
    const quint64 request = row == kBottomRow ? m_session->scrollViewportToBottom(m_renderConsumerToken)
                                              : m_session->scrollViewport(m_renderConsumerToken, row);
    if (request != 0) m_scrollRequest = request;
}

void TerminalView::advanceScrollAnimation()
{
    if (!m_scroll.animating()) return;
    if (m_scroll.advance(m_scrollClock.elapsed())) {
        handleScrollMoved();
    }
    // Asks the window, not update(): an update() during this frame's sync can
    // be folded into it and stall the animation.
    if (m_scroll.animating() && window()) window()->update();
}

void TerminalView::syncScroll(const TerminalFrameSnapshot *previous)
{
    const TerminalFrameSnapshot *snapshot = m_presented ? m_presented->snapshot.get() : nullptr;
    if (!snapshot) return;
    const TerminalScrollbar &bar = snapshot->frame.scrollbar;
    // The alternate screen has no history: its total is one screen.
    const qint64 liveTop = std::max<qint64>(0, bar.total - bar.rows);
    const qint64 previousLiveTop = m_scroll.liveTop();
    bool moved = m_scroll.setRange(0, liveTop);

    // The core moved the viewport itself (a search or prompt jump, selection
    // autoscroll, input, a resize): its position wins. Otherwise, once the
    // core has seen the last request, a still view takes the frame's
    // position, which moves when full history drops its oldest rows.
    const bool jumped = snapshot->viewportJump != m_viewportJump;
    m_viewportJump = snapshot->viewportJump;
    const bool caughtUp = snapshot->scrollRequest >= m_scrollRequest;
    const bool differs = m_scroll.atBottom() ? !bar.atBottom() : qint64(std::floor(m_scroll.top())) != bar.offset;
    if ((jumped || (caughtUp && !m_scroll.animating() && differs)) && adoptFrameViewport()) {
        moved = false;
    }

    // Output arrived below the text being read: history grew or, once full,
    // shifted under a viewport the view did not move.
    const bool grew = liveTop > previousLiveTop
        || (previous && !jumped && previous->scrollRequest == snapshot->scrollRequest
            && bar.offset < previous->frame.scrollbar.offset);
    if (grew && !m_scroll.atBottom() && !m_unreadOutput) {
        m_unreadOutput = true;
        emit unreadOutputChanged();
    }
    if (moved) {
        handleScrollMoved();
    } else if (liveTop != previousLiveTop) {
        emit scrollChanged();
    }
}

bool TerminalView::adoptFrameViewport()
{
    const TerminalScrollbar &bar = frame().scrollbar;
    // Already where the core is: no request goes back.
    m_requestedRow = bar.atBottom() ? kBottomRow : bar.offset;
    const bool moved = bar.atBottom() ? m_scroll.scrollToBottom() : m_scroll.scrollTo(double(bar.offset));
    if (moved) handleScrollMoved();
    return moved;
}

QSGNode *TerminalView::updatePaintNode(QSGNode *oldNode, UpdatePaintNodeData *) {
    const TerminalCursorResolution cursorResolution = resolveCursor();
    const std::optional<Cursor> visualCursor = cursorResolution.visualCursor;
    const TerminalImeAnchor &imeAnchor = m_imeComposition.anchor();
    TerminalSceneGraph::PaintState state;
    state.sceneGeneration = m_sceneGeneration;
    state.graphicsRevision = m_graphicsRevision;
    state.contentPresentationSequence = m_contentPresentationSequence;
    state.lastSnapshotSequence = m_lastSnapshotSequence;
    state.viewportEpoch = m_viewportEpoch;
    const DrawnRows drawn = drawnRows();
    state.rows = &drawn.rows;
    state.liveRow = drawn.liveRow;
    state.blinkVisible = m_textBlinkLit;
    state.frame = &frame();
    // Paint with the metrics the presented rows were shaped for. After a font
    // or DPI change they differ from m_fontSet until the next prepared frame.
    const bool preparedMetrics = m_presented && m_presented->cellHeight > 0;
    state.cellWidth = preparedMetrics ? m_presented->cellWidth : cellWidth();
    state.cellHeight = preparedMetrics ? m_presented->cellHeight : cellHeight();
    state.baseline = preparedMetrics ? m_presented->baseline : m_fontSet.baseline();
    state.devicePixelRatio = preparedMetrics ? m_presented->devicePixelRatio
                                             : m_fontSet.devicePixelRatio();
    state.rowOffset = -drawn.fraction * state.cellHeight;
    state.itemWidth = width();
    state.selectionHighlights = drawnHighlights(frame().selection, drawn);
    state.selectionColor = m_selection;
    state.searchHighlights =
        searchActive() ? drawnHighlights(frame().searchMatches, drawn) : QVector<TerminalHighlight>();
    state.searchMatchColor = m_searchMatchColor;
    state.searchCurrentColor = m_searchCurrentColor;
    state.showCursor = !m_imeComposition.active()
        && m_session && m_session->running() && hasActiveFocus()
        && visualCursor && visualCursor->row >= 0
        && visualCursor->row < frame().rows
        && visualCursor->column >= 0
        && visualCursor->column < std::max(1, frame().columns);
    state.visualCursor = visualCursor;
    // The engine resolves the user's default against the child's DECSCUSR.
    state.cursorShape = frame().cursor.shape;
    // OSC 12 from the child overrides the theme's cursor colour until OSC 112.
    state.cursorColor = frame().cursor.color.isValid() ? frame().cursor.color : m_cursor;
    state.cursorOpacity = m_cursorOpacity;
    state.paintPreedit = window() && m_imeComposition.active() && hasActiveFocus()
        && m_session && m_session->running() && imeAnchor.isValid()
        && imeAnchor.cursor.row >= 0 && imeAnchor.cursor.row < frame().rows;
    state.traceEnabled = m_viewportTracePaints > 0 && TerminalViewportTrace::enabled();
    state.imeComposition = &m_imeComposition;
    state.foreground = m_foreground;
    state.background = m_background;
    state.preeditFont = m_fontSet.baseFont();
    state.window = window();

    TerminalSceneGraph::PaintResult result;
    QSGNode *root = TerminalSceneGraph::sync(oldNode, state, result);
    if (result.contentSequenceAdvanced) {
        m_contentFrames->pending.store(true, std::memory_order_release);
    }
    if (result.viewportTraceChanged) {
        --m_viewportTracePaints;
        traceViewport(QStringLiteral("view-paint"),
                      {{QStringLiteral("sequence"), qint64(m_lastSnapshotSequence)},
                       {QStringLiteral("epoch"), qint64(m_viewportEpoch)},
                       {QStringLiteral("rows"), result.rowCount},
                       {QStringLiteral("rowChildren"), result.rowChildCount},
                       {QStringLiteral("width"), width()},
                       {QStringLiteral("height"), height()}});
    }
    return root;
}

void TerminalView::updatePolish() {
    const qreal previousCellWidth = m_fontSet.cellWidth();
    const qreal previousCellHeight = m_fontSet.cellHeight();
    m_fontSet.setFont(m_font);
    m_fontSet.setDevicePixelRatio(window() ? window()->devicePixelRatio() : 1.0);
    const bool metricsDidChange = !qFuzzyCompare(previousCellWidth, m_fontSet.cellWidth())
        || !qFuzzyCompare(previousCellHeight, m_fontSet.cellHeight());
    if (metricsDidChange) {
        m_viewportLifecycle.setCellSize(
            QSizeF(m_fontSet.cellWidth(), m_fontSet.cellHeight()));
        emit metricsChanged();
    }
    publishRenderStyle();

    if (!m_session) {
        m_presented.reset();
        return;
    }
    if (!ownsRenderConsumer()) {
        return;
    }
    if (width() <= 0 || height() <= 0) {
        if (m_viewportLifecycle.state() == TerminalViewportLifecycle::State::Normal) {
            m_presented.reset();
        }
        return;
    }

    if (m_viewportLifecycle.state() != TerminalViewportLifecycle::State::Normal
        || !isVisible()) {
        return;
    }

    if (!m_session->running() && !m_session->launchAttempted()) {
        TerminalSession *session = m_session;
        const quint64 bindingGeneration = m_sessionBindingGeneration;
        QTimer::singleShot(0, this, [this, session, bindingGeneration]() {
            if (m_session != session || m_sessionBindingGeneration != bindingGeneration
                || !isVisible() || !ownsRenderConsumer()) {
                return;
            }
            if (session->running() || session->launchAttempted()) {
                return;
            }
            session->start();
        });
    }
    // Rows are laid out and shaped on the preparation thread. Here the GUI
    // thread only decides whether the newest prepared frame may replace the
    // presented one, which is a pointer swap.
    requestPreparation();
    const std::shared_ptr<const TerminalPreparedFrame> prepared = m_preparer->latest();
    bool presentedCursorChanged = false;
    bool cursorAnimationResetNeeded = false;
    bool compositionAnchorChanged = false;
    bool appliedSnapshot = false;
    bool awaitingMatchingFrame = false;
    if (prepared && prepared->snapshot && prepared != m_presented
        && prepared->styleGeneration == m_renderStyle.generation && m_presented
        && prepared->snapshot == m_presented->snapshot && prepared->snapshot->sequence == m_lastSnapshotSequence) {
        // The applied snapshot re-prepared for another style: the same
        // content, already acknowledged. (The cells shown at binding are not
        // applied yet and take the full path.)
        m_presented = prepared;
        update();
    } else if (prepared && prepared->snapshot && prepared != m_presented
               && prepared->styleGeneration == m_renderStyle.generation) {
        const TerminalFrameSnapshot &snapshot = *prepared->snapshot;
        const SnapshotDecision decision =
            acceptSnapshot(snapshot, m_lastSnapshotSequence, m_renderTransportEpoch, m_viewportEpoch, frame());
        if (decision.disposition == SnapshotDisposition::AwaitMatching) {
            awaitingMatchingFrame = true;
            m_session->acknowledgeFrame(m_renderConsumerToken, snapshot.sequence);
        } else if (decision.disposition == SnapshotDisposition::Apply) {
            // Keeps the previous frame alive for the cursor comparisons.
            const std::shared_ptr<const TerminalPreparedFrame> previous = m_presented;
            if (m_renderTransportEpoch != 0
                && snapshot.transportEpoch != m_renderTransportEpoch
                && m_imeComposition.active()) {
                clearImeComposition(true);
            }
            m_presented = prepared;
            static const Frame emptyFrame;
            const Frame &previousFrame = previous && previous->snapshot
                ? previous->snapshot->frame : emptyFrame;
            presentedCursorChanged = TerminalCursorAnimation::presentationChanged(
                previousFrame, frame());
            cursorAnimationResetNeeded = TerminalCursorAnimation::shouldReset(
                previousFrame, frame());
            // New terminal content counts toward the content frame rate;
            // unchanged rows keep their prepared pointers.
            const bool contentChanged = decision.rebuildAll || !previous
                || snapshot.graphicsRevision != m_graphicsRevision || prepared->rows != previous->rows;
            m_lastSnapshotSequence = snapshot.sequence;
            m_renderTransportEpoch = snapshot.transportEpoch;
            m_graphicsRevision = snapshot.graphicsRevision;
            if (contentChanged) {
                m_contentPresentationSequence = snapshot.sequence;
            }
            const TerminalFrameSnapshot *previousSnapshot = previous ? previous->snapshot.get() : nullptr;
            syncScroll(previousSnapshot);
            if (!previousSnapshot || previousSnapshot->frame.hasSelection != snapshot.frame.hasSelection) {
                emit selectionChanged();
            }
            if (!previousSnapshot || previousSnapshot->search.needle != snapshot.search.needle
                || previousSnapshot->search.total != snapshot.search.total
                || previousSnapshot->search.selected != snapshot.search.selected
                || previousSnapshot->search.complete != snapshot.search.complete) {
                emit searchChanged();
            }
            compositionAnchorChanged = reanchorComposition();
            appliedSnapshot = true;
            m_session->acknowledgeFrame(m_renderConsumerToken, snapshot.sequence);
        }
    }

    if (m_viewportTraceFrames > 0 && TerminalViewportTrace::enabled()
        && (appliedSnapshot || awaitingMatchingFrame)) {
        --m_viewportTraceFrames;
        auto details = TerminalViewportTrace::describeFrame(frame());
        details.insert(QStringLiteral("glyphs"), prepared->glyphCount);
        details.insert(QStringLiteral("epoch"), qint64(m_viewportEpoch));
        details.insert(QStringLiteral("snapshotEpoch"), qint64(prepared->snapshot->viewportEpoch));
        details.insert(QStringLiteral("snapshotSequence"), qint64(prepared->snapshot->sequence));
        details.insert(QStringLiteral("presentedSequence"), qint64(m_lastSnapshotSequence));
        details.insert(QStringLiteral("waiting"), awaitingMatchingFrame);
        traceViewport(QStringLiteral("view-layout"), details);
    }
    if (cursorAnimationResetNeeded) {
        resetCursorAnimation();
    }
    const Qt::InputMethodQueries imeChanges =
        TerminalImeCompositionState::frameQueryChanges(
            m_imeComposition.active(), compositionAnchorChanged,
            presentedCursorChanged, metricsDidChange);
    if (imeChanges) {
        updateInputMethodQueries(imeChanges);
    }
    updateCursorAnimation();
    // Only a frame in flight needs another polish; queueing unconditionally
    // keeps the render loop spinning while idle.
    if (appliedSnapshot || awaitingMatchingFrame || cursorAnimationResetNeeded) {
        update();
    }
}

void TerminalView::geometryChange(const QRectF &newGeometry, const QRectF &oldGeometry) {
    QQuickItem::geometryChange(newGeometry, oldGeometry);
    if (newGeometry.size() != oldGeometry.size()) {
        m_viewportLifecycle.setContentSize(newGeometry.size());
        if (m_viewportLifecycle.state() == TerminalViewportLifecycle::State::Normal) {
            commitViewport();
        }
    }
    if (newGeometry != oldGeometry) {
        updateInputMethodQueries();
    }
}

void TerminalView::invalidateAll() {
    publishRenderStyle();
    requestPreparation();
    scheduleFrameUpdate();
}

const Frame &TerminalView::frame() const {
    static const Frame emptyFrame;
    return m_presented && m_presented->snapshot ? m_presented->snapshot->frame : emptyFrame;
}

const Line *TerminalView::lineAt(qint64 row) const
{
    const Frame &presented = frame();
    // Absolute row of the frame's first line.
    const qint64 index = row - (presented.scrollbar.offset - presented.viewportRow);
    return index >= 0 && index < presented.lines.size() ? &presented.lines.at(index) : nullptr;
}

TerminalView::DrawnRows TerminalView::drawnRows() const
{
    if (!m_presented || !m_presented->snapshot) return {};
    const double top = m_scroll.top();
    DrawnRows drawn = rowsFrom(top);
    // A position past the overscan (scrollbar dragged ahead of the core)
    // shows the nearest rows the frame has. Falling back to the frame's own
    // viewport makes the text shake while dragging through heavy output.
    if (drawn.missing == 0) return drawn;
    const Frame &presented = frame();
    const double first = double(presented.scrollbar.offset - presented.viewportRow);
    const double last = first + double(m_presented->rows.size() - presented.rows);
    if (last < first) return drawn;
    const double nearest = std::clamp(top, first, last);
    if (nearest == top) return drawn;
    DrawnRows clamped = rowsFrom(nearest);
    return clamped.missing < drawn.missing ? clamped : drawn;
}

TerminalView::DrawnRows TerminalView::rowsFrom(double top) const
{
    DrawnRows drawn;
    const Frame &presented = frame();
    drawn.firstLine = qint64(std::floor(top));
    drawn.fraction = top - double(drawn.firstLine);
    drawn.firstIndex = drawn.firstLine - (presented.scrollbar.offset - presented.viewportRow);
    drawn.liveRow = int(presented.scrollbar.offset - drawn.firstLine);
    const int count = presented.rows + (drawn.fraction > 0 ? 1 : 0);
    drawn.rows.reserve(count);
    for (qint64 index = drawn.firstIndex; index < drawn.firstIndex + count; ++index) {
        drawn.rows.push_back(index >= 0 && index < m_presented->rows.size() ? m_presented->rows.at(index) : nullptr);
        drawn.missing += drawn.rows.constLast() ? 0 : 1;
    }
    return drawn;
}

QVector<TerminalHighlight> TerminalView::drawnHighlights(const QVector<TerminalHighlight> &highlights,
                                                         const DrawnRows &drawn)
{
    QVector<TerminalHighlight> result;
    for (TerminalHighlight highlight : highlights) {
        const qint64 row = highlight.line - drawn.firstIndex;
        if (row < 0 || row >= drawn.rows.size()) continue;
        highlight.line = int(row);
        result.push_back(highlight);
    }
    return result;
}

bool TerminalView::hasBlinkingText() const {
    return m_presented && m_presented->hasBlink;
}

void TerminalView::publishRenderStyle() {
    const TerminalFontSet &posted = m_renderStyle.fonts;
    if (m_renderStyle.generation != 0
        && posted.baseFont() == m_fontSet.baseFont()
        && qFuzzyCompare(posted.devicePixelRatio(), m_fontSet.devicePixelRatio())
        && m_renderStyle.foreground == m_foreground
        && m_renderStyle.background == m_background
        && m_renderStyle.ansiPalette == m_ansiPalette) {
        return;
    }
    ++m_renderStyle.generation;
    m_renderStyle.fonts = m_fontSet;
    m_renderStyle.foreground = m_foreground;
    m_renderStyle.background = m_background;
    m_renderStyle.ansiPalette = m_ansiPalette;
    m_preparer->setStyle(m_renderStyle);
}

void TerminalView::requestPreparation() {
    if (!ownsRenderConsumer()) {
        return;
    }
    std::shared_ptr<const TerminalFrameSnapshot> snapshot = m_session->latestSnapshotShared();
    if (!snapshot || snapshot->sequence == 0 || snapshot == m_submittedSnapshot) {
        return;
    }
    m_submittedSnapshot = snapshot;
    m_preparer->submit(std::move(snapshot));
}

void TerminalView::scheduleFrameUpdate() {
    polish();
    update();
}

TerminalCursorResolution TerminalView::resolveCursor() const {
    if (!ownsRenderConsumer() || !m_session->running()) {
        return {};
    }
    return TerminalCursorResolver::resolve(frame(), m_renderTransportEpoch,
                                           m_scroll.atBottom() && frame().scrollbar.atBottom());
}

void TerminalView::handleFrameReady(quint64 sequence) {
    if (!ownsRenderConsumer() || sequence <= m_lastSnapshotSequence) {
        return;
    }
    // The preparation thread lays the frame out; its prepared() signal
    // schedules the polish that presents it.
    requestPreparation();
}

void TerminalView::handleTransportEpochChanged() {
    if (!m_session) {
        return;
    }

    clearImeComposition(true);
    // A new child gets a new engine: its rows, selection and viewport start
    // over, and the view follows its screen.
    resetViewportForLiveSession();
    resetCursorAnimation();
    updateCursorAnimation();
}

void TerminalView::resetViewportForLiveSession() {
    // A new session or a restarted process restarts row ids and revisions
    // with its engine, so the previous QSG tree must not survive.
    ++m_sceneGeneration;
    m_scroll.reset();
    m_requestedRow = kNoRow;
    m_scrollRequest = 0;
    m_viewportJump = 0;
    m_selecting = false;
    m_wheelStepRemainder = 0;
    m_protocolWheelStepRemainder = 0;
    m_protocolWheelColumnRemainder = 0;
    m_wheelVelocityClock.invalidate();
    m_wheelVelocity = 1.0;
    m_lastWheelDirection = 0.0;
    m_lastSnapshotSequence = 0;
    m_renderTransportEpoch = 0;
    m_contentPresentationSequence = 0;
    m_viewportLifecycle.reset();
    m_viewportEpoch = 0;
    const bool keepPresentedFrame = m_viewportLifecycle.state()
        != TerminalViewportLifecycle::State::Normal;
    if (!keepPresentedFrame) {
        m_presented.reset();
        m_graphicsRevision = 0;
    }
    // Snapshots of the previous binding or process must not be prepared or
    // serve as the reuse baseline for the new one.
    m_submittedSnapshot.reset();
    m_preparer->reset();
    m_imeComposition.clear();
    emit selectionChanged();
    if (m_unreadOutput) {
        m_unreadOutput = false;
        emit unreadOutputChanged();
    }
    emit scrollChanged();
    commitViewport();
    invalidateAll();
}

void TerminalView::syncRenderConsumer() {
    const QWindow::Visibility visibility = window() ? window()->visibility()
                                                     : QWindow::Hidden;
    const TerminalViewportLifecycle::State state = m_viewportLifecycle.state();
    const bool presentationAvailable = state == TerminalViewportLifecycle::State::Normal
        || state == TerminalViewportLifecycle::State::Moving;
    const bool active = ownsRenderConsumer() && isVisible() && window() && presentationAvailable
        && visibility != QWindow::Hidden && visibility != QWindow::Minimized;
    if (active == m_renderConsumerActive) {
        return;
    }
    m_renderConsumerActive = active;
    traceViewport(QStringLiteral("view-consumer"),
        {{QStringLiteral("active"), active}, {QStringLiteral("visible"), isVisible()},
         {QStringLiteral("windowVisibility"), int(visibility)},
         {QStringLiteral("lifecycle"), int(state)},
         {QStringLiteral("presentedSequence"), qint64(m_lastSnapshotSequence)}});
    if (m_session) {
        m_session->setRenderConsumer(m_renderConsumerToken, active, m_maximumFrameRate);
    }
    if (active) {
        commitViewport();
    }
}

bool TerminalView::ownsRenderConsumer() const {
    return m_session && m_session->ownsRenderConsumer(m_renderConsumerToken);
}

void TerminalView::updateCursorAnimation() {
    const bool interactionActive = isVisible() && window() && window()->isActive()
        && hasActiveFocus() && m_session && m_session->running();

    const bool textBlinkActive = interactionActive && hasBlinkingText();
    if (textBlinkActive) {
        if (!m_textBlinkTimer.isActive()) {
            m_textBlinkTimer.start();
        }
    } else {
        m_textBlinkTimer.stop();
        if (!m_textBlinkLit) {
            m_textBlinkLit = true;
            update();
        }
    }

    const TerminalCursorResolution resolved = resolveCursor();
    const bool cursorBlinkActive = TerminalCursorAnimation::shouldAnimate(
        resolved.visualCursor, m_scroll.atBottom(), m_session && m_session->running(), isVisible(),
        window() && window()->isActive(), hasActiveFocus(), m_imeComposition.active());
    const bool contentBusy = m_terminalContentFps >= kBusyContentFps;
    const bool withinHold = m_cursorHoldClock.isValid()
        && TerminalCursorAnimation::withinHold(m_cursorHoldClock.elapsed());
    if (TerminalCursorAnimation::shouldRunFadeTimer(
            cursorBlinkActive, contentBusy, withinHold)) {
        const int interval = m_cursorAnimationEnabled ? kCursorSmoothTickMs : kCursorDiscreteTickMs;
        const Qt::TimerType timerType = m_cursorAnimationEnabled
            ? Qt::PreciseTimer : Qt::CoarseTimer;
        if (m_cursorAnimationTimer.interval() != interval
            || m_cursorAnimationTimer.timerType() != timerType) {
            m_cursorAnimationTimer.setTimerType(timerType);
            m_cursorAnimationTimer.setInterval(interval);
        }
        if (!m_cursorAnimationTimer.isActive()) {
            if (m_cursorAnimationEnabled && !m_cursorAnimationClock.isValid()) {
                m_cursorAnimationClock.start();
            }
            m_cursorAnimationTimer.start();
        }
    } else {
        m_cursorAnimationTimer.stop();
        m_cursorOpacity = 1.0;
        m_cursorDiscreteLit = true;
        m_cursorAnimationClock.invalidate();
    }
}

void TerminalView::resetCursorAnimation() {
    m_cursorAnimationTimer.stop();
    m_cursorAnimationClock.invalidate();
    m_cursorOpacity = 1.0;
    m_cursorDiscreteLit = true;
}

void TerminalView::noteCursorHoldRestart() {
    m_cursorHoldClock.start();
    m_cursorHoldTimer.start(kCursorBlinkHoldMs);
    updateCursorAnimation();
}

void TerminalView::sampleContentFrameRate() {
    const qint64 elapsed = m_frameRateClock.restart();
    if (elapsed <= 0) {
        return;
    }
    const quint64 frames = m_contentFrames->frames.load(std::memory_order_relaxed);
    const int fps = qRound(double(frames - m_sampledContentFrames) * 1000.0 / double(elapsed));
    m_sampledContentFrames = frames;
    if (m_terminalContentFps == fps) {
        return;
    }
    m_terminalContentFps = fps;
    updateCursorAnimation();
}

void TerminalView::syncFrameRateSampling()
{
    // Frames are counted only when this view can be seen; a hidden or
    // minimized view samples nothing and reads as idle content.
    const QWindow::Visibility visibility = window() ? window()->visibility() : QWindow::Hidden;
    const bool sampling = isVisible() && visibility != QWindow::Hidden && visibility != QWindow::Minimized;
    if (sampling == m_frameRateTimer.isActive()) {
        return;
    }
    resetContentFrameRate();
    if (sampling) {
        m_frameRateTimer.start();
    } else {
        m_frameRateTimer.stop();
    }
    updateCursorAnimation();
}

void TerminalView::resetContentFrameRate() {
    m_sampledContentFrames = m_contentFrames->frames.load(std::memory_order_relaxed);
    m_contentFrames->pending.store(false, std::memory_order_release);
    m_frameRateClock.restart();
    m_terminalContentFps = 0;
}

} // namespace span::terminal
