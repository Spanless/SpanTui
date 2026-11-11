#pragma once

#include <QObject>
#include <QPointer>
#include <QSize>
#include <QTimer>

class QQuickWindow;

namespace span::application {

class WindowTransitionCoordinator final : public QObject {
    Q_OBJECT
    Q_PROPERTY(Phase phase READ phase NOTIFY phaseChanged FINAL)
    // Effects stay frozen while a transition is active.
    Q_PROPERTY(bool active READ active NOTIFY phaseChanged FINAL)

public:
    enum Phase {
        Stable,
        InteractiveResize,
        StateChange,
        Minimized,
        Settling,
        Recovering,
    };
    Q_ENUM(Phase)

    explicit WindowTransitionCoordinator(QObject *parent = nullptr);

    void attachWindow(QQuickWindow *window);

    Phase phase() const;
    bool active() const;
    quint64 epoch() const;
    quint64 commitRevision() const;
    void setStallWatchdogIntervalForTesting(int milliseconds);

    Q_INVOKABLE void beginLocalResize();
    Q_INVOKABLE void endLocalResize();
    Q_INVOKABLE void previewLocalViewport(int width, int height);

public slots:
    void beginNativeResize();
    void endNativeResize();
    void noteNativeStateChange(int nativeState);
    void noteDpiChange();

signals:
    void phaseChanged();
    void epochChanged();
    void viewportPreviewed(int width, int height, quint64 epoch);
    void viewportCommitted(int width, int height, quint64 epoch);

private:
    void beginTransition(Phase phase);
    void noteGeometry();
    void enterSettling(Phase phase = Settling);
    void commitPendingViewport();
    void setPhase(Phase phase);
    void recoverFromStalledTransition();

    QPointer<QQuickWindow> m_window;
    QTimer m_settleTimer;
    // Fires when an interactive resize or minimize transaction stops
    // receiving events, so effects can never stay frozen forever.
    QTimer m_stallWatchdog;
    QSize m_pendingSize;
    QSize m_committedSize;
    QSize m_localViewportSize;
    quint64 m_epoch = 0;
    quint64 m_commitRevision = 0;
    int m_localResizeDepth = 0;
    bool m_nativeResizeActive = false;
    Phase m_phase = Stable;
};

} // namespace span::application
