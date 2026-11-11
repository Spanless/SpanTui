#pragma once

#include "terminal/api/terminal_session.h"

#include <QObject>
#include <QColor>
#include <QHash>
#include <QList>
#include <QVector>
#include <QVariantList>
#include <QVariantMap>

namespace span::terminal {

// One catalog entry handed to TerminalManager::synchronize().
struct TerminalSessionSpec {
    QString id;
    QString cwd;
    // "" starts the default shell.
    QString commandLine;
    QVariantMap environment;
};

class TerminalManager final : public QObject {
    Q_OBJECT
    // QML-facing types are namespace-qualified so the QML type registrar
    // resolves them.
    Q_PROPERTY(span::terminal::TerminalSession *activeSession READ activeSession NOTIFY activeSessionChanged FINAL)

public:
    explicit TerminalManager(QObject *parent = nullptr);
    ~TerminalManager() override;

    TerminalSession *activeSession() const;
    int sessionCount() const;
    int runningSessionCount() const;
    qsizetype pendingInputBytes() const;
    qsizetype pendingOutputBytes() const;
    QList<TerminalSession *> sessions() const;
    template <typename Visitor>
    void forEachSession(Visitor &&visitor) const
    {
        for (auto *session : m_sessions) {
            visitor(session);
        }
    }

    Q_INVOKABLE span::terminal::TerminalSession *createSession(const QString &id, const QString &cwd,
                                                               const QVariantMap &options = {});
    Q_INVOKABLE span::terminal::TerminalSession *session(const QString &id) const;
    Q_INVOKABLE void closeSession(const QString &id);
    Q_INVOKABLE void setActiveSession(const QString &id);
    Q_INVOKABLE void restartSession(const QString &id);
    // Creates the listed sessions that do not exist yet and closes the ones
    // no longer listed.
    void synchronize(const QVector<TerminalSessionSpec> &sessions);
    // Live OSC / shell title when set; empty string if the session is missing
    // or has not published a title yet.
    Q_INVOKABLE QString sessionTitle(const QString &id) const;
    // Protocol surface colors for every session. Applied at create time so
    // COLORFGBG and OSC 10/11/4 are valid before a TerminalView binds.
    void setSurfaceColors(const QColor &foreground,
                          const QColor &background,
                          const QVector<QColor> &ansiPalette);

signals:
    void activeSessionChanged();
    void sessionStatesChanged();
    void errorOccurred(const QString &message);
    // A session's child asked for a desktop notification (OSC 9 / OSC 777).
    void notificationRequested(const QString &sessionId, const QString &title,
                               const QString &body);
    // A session's OSC 9;4 progress changed, or a session with progress closed.
    void progressChanged(const QString &sessionId);

private:
    void connectSession(TerminalSession *session);

    void applySurfaceColors(TerminalSession *session) const;

    QHash<QString, TerminalSession *> m_sessions;
    QString m_activeId;
    QColor m_surfaceForeground;
    QColor m_surfaceBackground;
    QVector<QColor> m_ansiPalette;
};

} // namespace span::terminal
