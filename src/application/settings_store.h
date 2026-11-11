#pragma once

#include <QList>
#include <QObject>
#include <QPair>
#include <QRect>
#include <QTimer>

class SessionModel;

namespace span::application {

// The settings table and session catalog of span_tui.db, plus the workspace
// state kept there (window geometry, layout). Preference objects read their
// keys after open() and stage changes with setValue(); staged values are
// written together after a short delay, on flush() or on destruction.
class SettingsStore final : public QObject {
    Q_OBJECT

public:
    enum class Mode {
        Workspace,
        // Never writes; without a database every key reads as its default.
        ReadOnly,
    };

    explicit SettingsStore(QObject *parent = nullptr);
    ~SettingsStore() override;

    // Workspace mode creates and migrates the database, then loads the
    // session catalog into the session model when one is set.
    bool open(Mode mode = Mode::Workspace);
    bool readOnly() const { return m_readOnly; }
    QString databasePath() const { return m_databasePath; }
    void setDatabasePathForTesting(const QString &path) { m_databasePath = path; }

    // Staged values win over stored ones.
    QString value(const QString &key, const QString &fallback = QString()) const;
    void setValue(const QString &key, const QString &value);
    // Writes every staged value in one transaction; on failure they stay
    // staged and the error is reported.
    bool flush();
    // Flushes settings and rewrites the session catalog.
    void saveNow();
    int persistenceQueueDepth() const { return m_pending.isEmpty() ? 0 : 1; }

    QString workspaceLayoutJson() const;
    void setWorkspaceLayoutJson(const QString &json);
    // Invalid until a valid geometry was saved.
    QRect windowGeometry() const;
    bool windowMaximized() const;
    // Written at once and as one batch; an invalid rectangle keeps the
    // stored one and only records the maximized flag.
    void saveWindowGeometry(int x, int y, int width, int height, bool maximized);

    void setSessionModel(SessionModel *model);
    bool saveSessionsNow();

signals:
    void errorOccurred(const QString &message);

private:
    class Transaction;

    bool openDatabase();
    bool loadSessions();
    bool saveSessions();
    bool saveActiveSessionFlags();
    bool writeValue(const QString &key, const QString &value);

    QString m_databasePath;
    QString m_connectionName;
    QList<QPair<QString, QString>> m_pending;
    QTimer m_saveTimer;
    SessionModel *m_sessionModel = nullptr;
    bool m_open = false;
    bool m_readOnly = false;
    // Refuse destructive rewrite saves until the matching load succeeded.
    bool m_sessionsLoaded = false;
};

} // namespace span::application
