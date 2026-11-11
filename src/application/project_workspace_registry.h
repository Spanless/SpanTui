#pragma once

#include <QAbstractListModel>
#include <QHash>
#include <QPointer>
#include <QSet>
#include <QUrl>

namespace span::application {

class ProjectContext;

class ProjectWorkspaceRegistry final : public QAbstractListModel {
    Q_OBJECT
    Q_MOC_INCLUDE("application/project_context.h")
    Q_PROPERTY(int count READ count NOTIFY projectsChanged FINAL)
    Q_PROPERTY(bool needsInitialProject READ needsInitialProject NOTIFY projectsChanged FINAL)

public:
    enum Role {
        NameRole = Qt::UserRole + 1,
        PathRole,
        ExpandedRole,
    };

    explicit ProjectWorkspaceRegistry(QString databasePath, QObject *parent = nullptr);
    ~ProjectWorkspaceRegistry() override;

    int rowCount(const QModelIndex &parent = {}) const override;
    QVariant data(const QModelIndex &index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

    int count() const;
    bool needsInitialProject() const;
    QStringList projectPaths() const;
    bool contains(const QString &projectPath) const;
    QString canonicalProjectPath(const QString &path) const;

    Q_INVOKABLE bool addProject(const QString &path);
    Q_INVOKABLE bool addProjectUrl(const QUrl &url);
    Q_INVOKABLE bool removeProject(const QString &projectPath);
    Q_INVOKABLE bool setExpanded(const QString &projectPath, bool expanded);
    ProjectContext *contextForProject(const QString &projectPath);
    void setIdle(bool idle);
    // Applied to every project context, now and when created. On by default.
    void setGitEnabled(bool enabled);
    // Projects some tile or the right dock shows; every other context is
    // background (no Git polling). Pushed by WorkspaceController, which owns
    // the set; applied to contexts now and when created.
    void setForegroundProjects(const QStringList &projectPaths);

signals:
    void projectsChanged();
    void errorOccurred(const QString &message);
    void contextCreated(span::application::ProjectContext *context);

private:
    struct Project {
        QString name;
        QString path;
        bool expanded = true;
    };

    int indexForPath(const QString &projectPath) const;
    bool load();
    bool upsert(const Project &project);
    bool touchLastOpened(const QString &projectPath);

    QString m_databasePath;
    QString m_connectionName;
    QVector<Project> m_projects;
    QHash<QString, QPointer<ProjectContext>> m_contexts;
    // Canonical paths, as m_contexts keys them.
    QSet<QString> m_foregroundProjects;
    bool m_idle = false;
    bool m_gitEnabled = true;
};

} // namespace span::application
