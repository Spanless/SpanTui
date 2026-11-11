#pragma once

#include <QObject>
#include <QTimer>

class QGuiApplication;

namespace span::application {

// Owns the "user idle" fact: the application is idle while its window is
// not active or no keyboard/mouse input arrived within the input timeout.
// Consumers (ResourceMonitor, per-project GitRepositoryService) subscribe
// to idleChanged and back off their background work.
class IdleGovernor final : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool idle READ idle NOTIFY idleChanged FINAL)

public:
    explicit IdleGovernor(QObject *parent = nullptr);

    bool idle() const;
    // Ignores ms <= 0. A running countdown restarts with the new timeout;
    // otherwise the next activity starts it.
    void setInputTimeoutMs(int ms);

    void attach(QGuiApplication *app);

    // Public for tests; production input arrives through the event filter
    // and applicationStateChanged.
    void noteActivity();
    void setApplicationActive(bool active);

    bool eventFilter(QObject *watched, QEvent *event) override;

signals:
    void idleChanged();

private:
    void recomputeIdle();

    QTimer m_inputTimer;
    int m_inputTimeoutMs = 30000;
    bool m_applicationActive = true;
    bool m_inputTimedOut = false;
    bool m_idle = false;
};

} // namespace span::application
