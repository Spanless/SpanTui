#include "terminal/api/terminal_manager.h"

#include "terminal/api/terminal_session.h"
#include "terminal/terminal_constants.h"

#include <QSet>
#include <algorithm>

namespace span::terminal {

TerminalManager::TerminalManager(QObject *parent) : QObject(parent) {}

TerminalManager::~TerminalManager() {
    qDeleteAll(m_sessions);
}

TerminalSession *TerminalManager::activeSession() const {
    return m_sessions.value(m_activeId, nullptr);
}

int TerminalManager::sessionCount() const { return m_sessions.size(); }

QList<TerminalSession *> TerminalManager::sessions() const {
    return m_sessions.values();
}

int TerminalManager::runningSessionCount() const {
    int count = 0;
    for (const auto *session : m_sessions) {
        count += session && session->running() ? 1 : 0;
    }
    return count;
}

qsizetype TerminalManager::pendingInputBytes() const {
    qsizetype total = 0;
    for (const auto *session : m_sessions) {
        total += session->pendingInputBytes();
    }
    return total;
}

qsizetype TerminalManager::pendingOutputBytes() const {
    qsizetype total = 0;
    for (const auto *session : m_sessions) {
        total += session->pendingOutputBytes();
    }
    return total;
}

TerminalSession *TerminalManager::createSession(const QString &id,
                                                const QString &cwd,
                                                const QVariantMap &options) {
    if (id.isEmpty()) {
        return nullptr;
    }
    if (auto *existing = session(id)) {
        return existing;
    }
    auto *created = new TerminalSession(id, cwd, options, this);
    applySurfaceColors(created);
    m_sessions.insert(id, created);
    connectSession(created);
    emit sessionStatesChanged();
    return created;
}

TerminalSession *TerminalManager::session(const QString &id) const {
    return m_sessions.value(id, nullptr);
}

void TerminalManager::closeSession(const QString &id) {
    auto *removed = m_sessions.take(id);
    if (!removed) {
        return;
    }
    const bool wasActive = m_activeId == id;
    if (wasActive) {
        m_activeId.clear();
    }
    // Explicit close kills this terminal's ConPTY process tree.
    removed->stop();
    const bool hadProgress = removed->progressState() != 0;
    delete removed;
    if (hadProgress) {
        emit progressChanged(id);
    }
    if (wasActive) {
        emit activeSessionChanged();
    }
    emit sessionStatesChanged();
}

void TerminalManager::setActiveSession(const QString &id) {
    const QString next = m_sessions.contains(id) ? id : QString();
    if (m_activeId == next) {
        // Routing is idempotent. Restarting an exited process is an explicit
        // lifecycle action and must never happen because catalog metadata was
        // synchronized or translated.
        return;
    }
    // Keep the previous PTY alive. Switching tabs only changes which session
    // is focused for display/input — background terminals continue running.
    m_activeId = next;
    if (TerminalSession *target = session(m_activeId)) {
        if (!target->launchAttempted()) {
            target->start();
        }
    }
    emit activeSessionChanged();
    emit sessionStatesChanged();
}

void TerminalManager::restartSession(const QString &id) {
    TerminalSession *target = session(id);
    if (!target) {
        return;
    }
    if (m_activeId != id) {
        setActiveSession(id);
    }
    if (target->running()) {
        return;
    }
    target->start();
    emit sessionStatesChanged();
}

void TerminalManager::synchronize(const QVector<TerminalSessionSpec> &sessions)
{
    QSet<QString> seen;
    for (const TerminalSessionSpec &spec : sessions) {
        if (spec.id.isEmpty()) {
            continue;
        }
        seen.insert(spec.id);
        QVariantMap options;
        if (!spec.commandLine.isEmpty()) {
            options.insert(kSessionCommandLineKey, spec.commandLine);
        }
        if (!spec.environment.isEmpty()) {
            options.insert(kSessionEnvironmentKey, spec.environment);
        }
        if (auto *existing = session(spec.id)) {
            // A one-shot initial command is only listed once; a command the
            // catalog keeps listing (a remote host whose endpoint can be
            // edited) applies to the next start.
            if (!spec.commandLine.isEmpty()) {
                existing->setLaunchOptions(options);
            }
            continue;
        }
        createSession(spec.id, spec.cwd, options);
    }

    const QStringList existing = m_sessions.keys();
    for (const QString &id : existing) {
        if (!seen.contains(id)) {
            closeSession(id);
        }
    }
    // Catalog synchronization never chooses focus or restarts a process.
    // SessionModel::activeSessionIdChanged is the sole routing trigger.
}

QString TerminalManager::sessionTitle(const QString &id) const {
    const auto *terminal = session(id);
    return terminal ? terminal->title() : QString();
}


void TerminalManager::setSurfaceColors(const QColor &foreground,
                                       const QColor &background,
                                       const QVector<QColor> &ansiPalette) {
    if (m_surfaceForeground == foreground && m_surfaceBackground == background
        && m_ansiPalette == ansiPalette) {
        return;
    }
    m_surfaceForeground = foreground;
    m_surfaceBackground = background;
    m_ansiPalette = ansiPalette;
    for (TerminalSession *session : std::as_const(m_sessions)) {
        applySurfaceColors(session);
    }
}

void TerminalManager::applySurfaceColors(TerminalSession *session) const {
    if (!session) {
        return;
    }
    session->setDefaultColors(m_surfaceForeground, m_surfaceBackground);
    session->setAnsiPalette(m_ansiPalette);
}

void TerminalManager::connectSession(TerminalSession *terminal) {
    connect(terminal, &TerminalSession::stateChanged, this,
            &TerminalManager::sessionStatesChanged);
    // Session tree labels follow the live title and activity.
    connect(terminal, &TerminalSession::titleChanged, this,
            &TerminalManager::sessionStatesChanged);
    connect(terminal, &TerminalSession::activityChanged, this,
            &TerminalManager::sessionStatesChanged);
    connect(terminal, &TerminalSession::errorOccurred, this, &TerminalManager::errorOccurred);
    connect(terminal, &TerminalSession::notificationRequested, this,
            [this, id = terminal->id()](const QString &title, const QString &body) {
                emit notificationRequested(id, title, body);
            });
    connect(terminal, &TerminalSession::progressChanged, this,
            [this, id = terminal->id()]() { emit progressChanged(id); });
}

} // namespace span::terminal
