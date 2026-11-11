#include "application/window_transition_coordinator.h"

#include <QQuickWindow>
#include <windows.h>

namespace span::application {

namespace {
constexpr int kSettleMilliseconds = 120;
// A drag that produces no geometry event for this long is treated as ended;
// without this a lost WM_EXITSIZEMOVE kept active() true forever.
// The window stays generous because a user pausing mid-gesture is
// indistinguishable from silence at this layer — a slow recovery is harmless,
// while a premature commit unfreezes effects during a real drag.
constexpr int kStallWatchdogMilliseconds = 4000;
}

WindowTransitionCoordinator::WindowTransitionCoordinator(QObject *parent)
    : QObject(parent)
{
    m_settleTimer.setSingleShot(true);
    m_settleTimer.setInterval(kSettleMilliseconds);
    connect(&m_settleTimer, &QTimer::timeout, this,
            &WindowTransitionCoordinator::commitPendingViewport);
    m_stallWatchdog.setSingleShot(true);
    m_stallWatchdog.setInterval(kStallWatchdogMilliseconds);
    connect(&m_stallWatchdog, &QTimer::timeout, this,
            &WindowTransitionCoordinator::recoverFromStalledTransition);
}

void WindowTransitionCoordinator::attachWindow(QQuickWindow *window)
{
    if (m_window == window) {
        return;
    }
    if (m_window) {
        disconnect(m_window, nullptr, this, nullptr);
    }
    m_window = window;
    if (!m_window) {
        m_settleTimer.stop();
        m_stallWatchdog.stop();
        m_nativeResizeActive = false;
        m_localResizeDepth = 0;
        setPhase(Stable);
        return;
    }


    connect(m_window, &QWindow::widthChanged, this,
            &WindowTransitionCoordinator::noteGeometry);
    connect(m_window, &QWindow::heightChanged, this,
            &WindowTransitionCoordinator::noteGeometry);
    connect(m_window, &QWindow::screenChanged, this,
            [this]() { noteDpiChange(); });
    connect(m_window, &QWindow::visibilityChanged, this,
            [this](QWindow::Visibility visibility) {
        if (visibility == QWindow::Minimized) {
            m_settleTimer.stop();
            m_nativeResizeActive = false;
            m_localResizeDepth = 0;
            m_stallWatchdog.start();
            beginTransition(Minimized);
            return;
        }
        if (m_phase == Minimized) {
            beginTransition(Recovering);
            noteGeometry();
        }
    });

    m_pendingSize = m_localViewportSize.isValid()
        ? m_localViewportSize : m_window->size();
    m_committedSize = m_pendingSize;
    ++m_epoch;
    emit epochChanged();
    emit viewportPreviewed(m_pendingSize.width(), m_pendingSize.height(), m_epoch);
    // Do not emit viewportCommitted here. QML may still be at the default
    // window size; the first durable commit waits for canvas preview/settle
    // so layout fill is not captured against the startup geometry.
    if (m_pendingSize.width() > 0 && m_pendingSize.height() > 0) {
        enterSettling();
    }
}

WindowTransitionCoordinator::Phase WindowTransitionCoordinator::phase() const { return m_phase; }
bool WindowTransitionCoordinator::active() const { return m_phase != Stable; }
quint64 WindowTransitionCoordinator::epoch() const { return m_epoch; }
quint64 WindowTransitionCoordinator::commitRevision() const { return m_commitRevision; }
void WindowTransitionCoordinator::setStallWatchdogIntervalForTesting(
    int milliseconds)
{
    m_stallWatchdog.setInterval(qMax(1, milliseconds));
}

void WindowTransitionCoordinator::beginLocalResize()
{
    ++m_localResizeDepth;
    if (m_localResizeDepth == 1) {
        beginTransition(InteractiveResize);
    }
    if (m_phase != Minimized) {
        m_stallWatchdog.start();
    }
}

void WindowTransitionCoordinator::endLocalResize()
{
    if (m_localResizeDepth <= 0) {
        return;
    }
    --m_localResizeDepth;
    if (m_localResizeDepth == 0 && !m_nativeResizeActive) {
        enterSettling();
    }
}

void WindowTransitionCoordinator::previewLocalViewport(int width, int height)
{
    if (width <= 0 || height <= 0) {
        return;
    }
    const QSize size(width, height);
    if (m_localViewportSize == size && m_window && m_pendingSize == size) {
        return;
    }
    m_localViewportSize = size;
    if (!m_window) {
        return;
    }
    m_pendingSize = size;
    if (m_phase == Stable) {
        beginTransition(StateChange);
    }
    emit viewportPreviewed(width, height, m_epoch);
    if (!m_nativeResizeActive && m_localResizeDepth == 0
        && m_phase != Minimized) {
        enterSettling(m_phase == Recovering ? Recovering : Settling);
    } else if (m_phase != Minimized) {
        m_stallWatchdog.start();
    }
}

void WindowTransitionCoordinator::beginNativeResize()
{
    m_nativeResizeActive = true;
    beginTransition(InteractiveResize);
    if (m_phase != Minimized) {
        m_stallWatchdog.start();
    }
}

void WindowTransitionCoordinator::endNativeResize()
{
    if (!m_nativeResizeActive) {
        return;
    }
    m_nativeResizeActive = false;
    if (m_localResizeDepth == 0) {
        enterSettling();
    }
}

void WindowTransitionCoordinator::noteNativeStateChange(int nativeState)
{
    // WM_SIZE uses 0=restored, 1=minimized and 2=maximized. WindowChrome
    // reports the native fact directly so Qt visibility propagation order
    // cannot misclassify a fast minimize/restore transition.
    if (nativeState == 1) {
        m_settleTimer.stop();
        m_nativeResizeActive = false;
        m_localResizeDepth = 0;
        m_stallWatchdog.start();
        beginTransition(Minimized);
        return;
    }
    beginTransition((m_nativeResizeActive || m_localResizeDepth > 0)
                        ? InteractiveResize
                        : ((m_phase == Minimized || m_phase == Recovering)
                               ? Recovering : StateChange));
    noteGeometry();
}

void WindowTransitionCoordinator::noteDpiChange()
{
    beginTransition((m_nativeResizeActive || m_localResizeDepth > 0)
                        ? InteractiveResize
                        : ((m_phase == Minimized || m_phase == Recovering)
                               ? Recovering : StateChange));
    noteGeometry();
}

void WindowTransitionCoordinator::beginTransition(Phase phase)
{
    const bool crossingMinimizedBoundary =
        (phase == Minimized) != (m_phase == Minimized);
    const bool startsNewEpoch = m_phase == Stable
        || crossingMinimizedBoundary
        || (phase == InteractiveResize && m_phase != InteractiveResize);
    if (startsNewEpoch) {
        ++m_epoch;
    }
    setPhase(phase);
    if (startsNewEpoch) {
        emit epochChanged();
    }
}

void WindowTransitionCoordinator::noteGeometry()
{
    if (!m_window) {
        return;
    }
    const QSize size = m_window->size();
    if (size.width() <= 0 || size.height() <= 0) {
        return;
    }
    if (m_phase == Stable) {
        beginTransition(StateChange);
    }
    // Canvas previewLocalViewport owns the layout viewport. Window size includes
    // chrome and must not overwrite that pending commit or preview layout.
    if (!m_localViewportSize.isValid()) {
        const bool sizeChanged = m_pendingSize != size;
        m_pendingSize = size;
        if (sizeChanged) {
            emit viewportPreviewed(size.width(), size.height(), m_epoch);
        }
    }
    if (!m_nativeResizeActive && m_localResizeDepth == 0
        && m_phase != Minimized) {
        enterSettling(m_phase == Recovering ? Recovering : Settling);
    } else if (m_phase != Minimized) {
        // Keep the stall watchdog alive while the drag keeps moving; it only
        // fires after a full silent interval.
        m_stallWatchdog.start();
    }
}


void WindowTransitionCoordinator::enterSettling(Phase phase)
{
    if (m_phase == Minimized) {
        return;
    }
    setPhase(phase);
    m_settleTimer.start();
    m_stallWatchdog.start();
}

void WindowTransitionCoordinator::recoverFromStalledTransition()
{
    if (!m_window) {
        return;
    }
    if (m_phase == Minimized) {
        if (m_window->visibility() != QWindow::Minimized) {
            m_nativeResizeActive = false;
            m_localResizeDepth = 0;
            beginTransition(Recovering);
            noteGeometry();
        } else {
            m_stallWatchdog.start();
        }
        return;
    }
    // While a native size/move drag runs, the OS holds the mouse capture on
    // this window. Silence with capture still held means the user paused
    // mid-gesture — keep watching rather than committing mid-drag.
    if (m_nativeResizeActive
        && ::GetCapture() == reinterpret_cast<HWND>(m_window->winId())) {
        m_stallWatchdog.start();
        return;
    }
    const bool stalled = m_nativeResizeActive || m_localResizeDepth > 0;
    m_nativeResizeActive = false;
    m_localResizeDepth = 0;
    if (stalled || m_phase != Stable) {
        qWarning("Window transition stalled without an end event; forcing a viewport commit.");
        commitPendingViewport();
    }
    if (m_phase != Stable) {
        // Commit was refused (invalid pending size); keep watching instead of
        // freezing effects indefinitely.
        m_stallWatchdog.start();
    }
}

void WindowTransitionCoordinator::commitPendingViewport()
{
    if (!m_window || m_nativeResizeActive || m_localResizeDepth > 0
        || m_phase == Minimized) {
        return;
    }
    if (m_pendingSize.width() <= 0 || m_pendingSize.height() <= 0) {
        return;
    }
    m_stallWatchdog.stop();
    m_committedSize = m_pendingSize;
    ++m_commitRevision;
    setPhase(Stable);
    emit viewportCommitted(m_committedSize.width(), m_committedSize.height(), m_epoch);
}

void WindowTransitionCoordinator::setPhase(Phase phase)
{
    if (m_phase == phase) {
        return;
    }
    m_phase = phase;
    emit phaseChanged();
}

} // namespace span::application
