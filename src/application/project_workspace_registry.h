#pragma once

#include <QAbstractListModel>
#include <QHash>
#include <QPointer>
#include <QSet>
#include <QUrl>
#include <QVariantList>

namespace span::application {

class ProjectContext;

// Registered projects. Rows are the top-level projects; a linked Git
// worktree of one of them is listed under it (WorktreesRole) instead of as a
// row of its own. Both kinds are full projects everywhere else (sessions,
// contexts, foreground set): projectPaths() and contains() cover them all.
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
        // The folder exists (as of the last refreshFolders()).
        AvailableRole,
        // Worktree projects listed under this one, ordered by name:
        // [{name, path, expanded, available, branch}].
        WorktreesRole,
        // What the folder has checked out (span::git::headLabelOf), as of
        // the last refreshFolders() or Git refresh of its context.
        BranchRole,
    };

    explicit ProjectWorkspaceRegistry(QString databasePath, QObject *parent = nullptr);
    ~ProjectWorkspaceRegistry() override;

    int rowCount(const QModelIndex &parent = {}) const override;
    QVariant data(const QModelIndex &index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

    // Every registered project, worktrees included.
    int count() const;
    bool needsInitialProject() const;
    QStringList projectPaths() const;
    bool contains(const QString &projectPath) const;
    QString canonicalProjectPath(const QString &path) const;
    // The stored path of a registered project, also when its folder is gone
    // (canonicalProjectPath() needs the folder); empty when not registered.
    QString registeredPath(const QString &path) const;
    Q_INVOKABLE bool isAvailable(const QString &projectPath) const;
    // The project a worktree project is listed under; empty for a top-level
    // or unknown project.
    Q_INVOKABLE QString parentProject(const QString &projectPath) const;
    // Worktree projects listed under a top-level project.
    QStringList worktreePaths(const QString &projectPath) const;
    // The folder is the top of a Git worktree (holds `.git`).
    Q_INVOKABLE bool isGitRepository(const QString &projectPath) const;
    // Linked worktrees of a top-level project that are not registered yet:
    // [{path, name, branch}], read from the repository's files.
    Q_INVOKABLE QVariantList unlistedWorktrees(const QString &projectPath) const;

    // A linked worktree of a registered top-level project is listed under
    // it; a new main project adopts its worktrees registered before it.
    Q_INVOKABLE bool addProject(const QString &path);
    Q_INVOKABLE bool addProjectUrl(const QUrl &url);
    // Drops the record (a top-level project takes its worktree entries
    // along); the folder is never touched.
    Q_INVOKABLE bool removeProject(const QString &projectPath);
    Q_INVOKABLE bool setExpanded(const QString &projectPath, bool expanded);
    // Reads again which project folders exist and what each has checked
    // out; called when the window is activated, before a project menu opens
    // and when a project context's Git state changes.
    Q_INVOKABLE void refreshFolders();
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
        // Top-level project this worktree is listed under; empty at top level.
        QString parentPath;
        bool expanded = true;
        bool available = true;
        // Not stored: read from the folder.
        QString branch;
        // Only on top-level projects.
        QVector<Project> worktrees;
    };
    struct Location {
        int row = -1;
        // -1 for the top-level project itself.
        int worktree = -1;
        bool valid() const { return row >= 0; }
    };

    Location locate(const QString &projectPath) const;
    const Project *project(const Location &location) const;
    Project *project(const Location &location);
    void notifyRow(int row, const QList<int> &roles);
    static void sortWorktrees(QVector<Project> &worktrees);
    bool load();
    bool upsert(const Project &project);
    bool deleteRows(const QString &projectPath, bool withWorktrees);
    bool touchLastOpened(const QString &projectPath);
    void dropContext(const QString &projectPath);

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
