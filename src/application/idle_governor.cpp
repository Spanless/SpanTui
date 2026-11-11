#include "application/idle_governor.h"

#include <QEvent>
#include <QGuiApplication>

namespace span::application {

IdleGovernor::IdleGovernor(QObject *parent)
    : QObject(parent)
{
    m_inputTimer.setSingleShot(true);
    m_inputTimer.setTimerType(Qt::CoarseTimer);
    m_inputTimer.setInterval(m_inputTimeoutMs);
    connect(&m_inputTimer, &QTimer::timeout, this, [this]() {
        m_inputTimedOut = true;
        recomputeIdle();
    });
    m_inputTimer.start();
}

bool IdleGovernor::idle() const { return m_idle; }

void IdleGovernor::setInputTimeoutMs(int ms)
{
    if (ms <= 0) {
        return;
    }
    m_inputTimeoutMs = ms;
    // Always store the interval: noteActivity() restarts the timer with it,
    // even when the change arrived after the input timeout had fired.
    m_inputTimer.setInterval(ms);
    if (m_inputTimer.isActive()) {
        m_inputTimer.start();
    }
}

void IdleGovernor::attach(QGuiApplication *app)
{
    if (!app) {
        return;
    }
    app->installEventFilter(this);
    setApplicationActive(app->applicationState() == Qt::ApplicationActive);
    connect(app, &QGuiApplication::applicationStateChanged, this,
            [this](Qt::ApplicationState state) {
        setApplicationActive(state == Qt::ApplicationActive);
    });
}

void IdleGovernor::noteActivity()
{
    m_inputTimedOut = false;
    m_inputTimer.start();
    recomputeIdle();
}

void IdleGovernor::setApplicationActive(bool active)
{
    if (m_applicationActive == active) {
        return;
    }
    m_applicationActive = active;
    if (active) {
        // Regaining the window counts as activity: the input clock restarts
        // instead of re-entering idle on a stale timeout.
        noteActivity();
        return;
    }
    recomputeIdle();
}

bool IdleGovernor::eventFilter(QObject *watched, QEvent *event)
{
    Q_UNUSED(watched);
    switch (event->type()) {
    case QEvent::KeyPress:
    case QEvent::KeyRelease:
    case QEvent::MouseButtonPress:
    case QEvent::MouseButtonRelease:
    case QEvent::MouseMove:
    case QEvent::Wheel:
    case QEvent::TouchBegin:
    case QEvent::TouchUpdate:
    case QEvent::TabletPress:
        noteActivity();
        break;
    default:
        break;
    }
    return false;
}

void IdleGovernor::recomputeIdle()
{
    const bool next = !m_applicationActive || m_inputTimedOut;
    if (next == m_idle) {
        return;
    }
    m_idle = next;
    emit idleChanged();
}

} // namespace span::application
