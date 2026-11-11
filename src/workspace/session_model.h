#pragma once

#include <QAbstractListModel>
#include <QHash>
#include <QVariantList>
#include <QVector>

// One stored entry of the session catalog, as records() returns it and
// load() takes it.
struct SessionRecord {
    QString id;
    QString name;
    QString cwd;
    // Scope key: a project path or "remote:<host id>" (workspace/scope.h).
    QString projectPath;
    QString shellProfile;
    bool active = false;
    // The session its scope focused last.
    bool projectActive = false;

    friend bool operator==(const SessionRecord &a, const SessionRecord &b)
    {
        return a.id == b.id && a.name == b.name && a.cwd == b.cwd && a.projectPath == b.projectPath
            && a.shellProfile == b.shellProfile && a.active == b.active && a.projectActive == b.projectActive;
    }
};

class SessionModel final : public QAbstractListModel {
    Q_OBJECT
    Q_PROPERTY(int activeIndex READ activeIndex NOTIFY activeIndexChanged FINAL)
    Q_PROPERTY(QString activeSessionId READ activeSessionId NOTIFY activeSessionIdChanged FINAL)
    Q_PROPERTY(int count READ count NOTIFY sessionsChanged FINAL)

public:
    enum Role { IdRole = Qt::UserRole + 1, NameRole, CwdRole, ProjectPathRole, ActiveRole };

    explicit SessionModel(QObject *parent = nullptr);

    int rowCount(const QModelIndex &parent = QModelIndex()) const override;
    QVariant data(const QModelIndex &index, int role = Qt::DisplayRole) const override;
    QHash<int, QByteArray> roleNames() const override;

    int activeIndex() const;
    QString activeSessionId() const;
    QString sessionIdAt(int row) const;
    Q_INVOKABLE QString projectPathForSession(const QString &id) const;
    Q_INVOKABLE QString nameForSessionId(const QString &id) const;
    Q_INVOKABLE int indexForSessionId(const QString &id) const;
    int count() const;
    // The catalog as persisted: stored names, one "active" row, and one
    // "projectActive" row per scope.
    QVector<SessionRecord> records() const;
    // Replaces the catalog; missing ids, names, directories and scopes are
    // filled in.
    void load(const QVector<SessionRecord> &records);
    QString takeInitialCommandLine(const QString &id);
    // Shell profile the session starts; "" follows the default profile.
    QString shellProfileForSession(const QString &id) const;

    Q_INVOKABLE QString addSession(const QString &name = QString(), const QString &cwd = QString(),
                                   const QString &projectPath = QString(), const QString &shellProfile = QString());
    Q_INVOKABLE void activateById(const QString &id);
    Q_INVOKABLE void removeSessionById(const QString &id);
    Q_INVOKABLE QString addCommandSession(const QString &name, const QString &cwd, const QString &projectPath,
                                          const QString &commandLine);
    Q_INVOKABLE void removeSession(int row);
    void activate(int row);
    Q_INVOKABLE void renameSession(int row, const QString &name);
    Q_INVOKABLE void retranslate();
    Q_INVOKABLE int countForProject(const QString &projectPath) const;
    int activeIndexForProject(const QString &projectPath) const;
    Q_INVOKABLE QVariantList sessionsForProject(const QString &projectPath) const;
    Q_INVOKABLE QVariantList sessionsForRemoteHost(const QString &hostId) const;

signals:
    void activeIndexChanged();
    void activeSessionIdChanged();
    void sessionsChanged();

private:
    struct Session {
        QString id;
        QString name;
        QString cwd;
        QString projectPath;
        QString initialCommandLine;
        QString shellProfile;
    };

    static QString displayName(const QString &storedName);
    static QString normalizePath(const QString &path);
    int indexForId(const QString &id) const;
    int firstIndexForProject(const QString &projectPath) const;
    QString insertSession(const QString &name, const QString &cwd, const QString &projectPath,
                          const QString &initialCommandLine, const QString &shellProfile = QString());
    void rememberActiveForProject(int row);
    void setActiveIndex(int row);

    QVector<Session> m_sessions;
    // Last focused session id per project. Project switches restore this instead
    // of always jumping to the first catalog entry of the destination project.
    QHash<QString, QString> m_lastActiveIdByProject;
    int m_activeIndex = -1;
};
