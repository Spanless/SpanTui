#include "application/project_workspace_registry.h"

#include "application/project_context.h"
#include "core/core_constants.h"
#include "git/git_worktree_layout.h"

#include <QDir>
#include <QUrl>
#include <QFileInfo>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QUuid>

#include <algorithm>

namespace span::application {

namespace {

bool samePath(const QString &a, const QString &b)
{
    return QDir::cleanPath(a).compare(QDir::cleanPath(b), Qt::CaseInsensitive) == 0;
}

QString folderName(const QString &canonical)
{
    const QString name = QFileInfo(canonical).fileName();
    return name.isEmpty() ? canonical : name;
}

} // namespace

ProjectWorkspaceRegistry::ProjectWorkspaceRegistry(QString databasePath,
                                                   QObject *parent)
    : QAbstractListModel(parent),
      m_databasePath(std::move(databasePath)),
      m_connectionName(QStringLiteral("project-registry-%1")
                           .arg(QUuid::createUuid().toString(QUuid::Id128)))
{
    load();
}

ProjectWorkspaceRegistry::~ProjectWorkspaceRegistry()
{
    {
        QSqlDatabase db = QSqlDatabase::database(m_connectionName, false);
        if (db.isValid()) {
            db.close();
        }
    }
    QSqlDatabase::removeDatabase(m_connectionName);
}

int ProjectWorkspaceRegistry::rowCount(const QModelIndex &parent) const
{
    return parent.isValid() ? 0 : m_projects.size();
}

QVariant ProjectWorkspaceRegistry::data(const QModelIndex &index, int role) const
{
    if (!index.isValid() || index.row() < 0 || index.row() >= m_projects.size()) {
        return {};
    }
    const Project &project = m_projects.at(index.row());
    switch (role) {
    case NameRole: return project.name;
    case PathRole: return project.path;
    case ExpandedRole: return project.expanded;
    case AvailableRole: return project.available;
    case BranchRole: return project.branch;
    case WorktreesRole: {
        QVariantList worktrees;
        worktrees.reserve(project.worktrees.size());
        for (const Project &worktree : project.worktrees) {
            worktrees.append(QVariantMap{{QStringLiteral("name"), worktree.name},
                                         {QStringLiteral("path"), worktree.path},
                                         {QStringLiteral("expanded"), worktree.expanded},
                                         {QStringLiteral("available"), worktree.available},
                                         {QStringLiteral("branch"), worktree.branch}});
        }
        return worktrees;
    }
    default: return {};
    }
}

QHash<int, QByteArray> ProjectWorkspaceRegistry::roleNames() const
{
    return {{NameRole, "name"},           {PathRole, "path"},           {ExpandedRole, "expanded"},
            {AvailableRole, "available"}, {WorktreesRole, "worktrees"}, {BranchRole, "branch"}};
}

int ProjectWorkspaceRegistry::count() const
{
    int total = m_projects.size();
    for (const Project &project : m_projects) {
        total += project.worktrees.size();
    }
    return total;
}

bool ProjectWorkspaceRegistry::needsInitialProject() const { return m_projects.isEmpty(); }

QStringList ProjectWorkspaceRegistry::projectPaths() const
{
    QStringList paths;
    paths.reserve(count());
    for (const Project &project : m_projects) {
        paths.append(project.path);
        for (const Project &worktree : project.worktrees) {
            paths.append(worktree.path);
        }
    }
    return paths;
}

QString ProjectWorkspaceRegistry::canonicalProjectPath(const QString &path) const
{
    QString local = path.trimmed();
    if (local.startsWith(QLatin1String("file:"), Qt::CaseInsensitive)) {
        local = QUrl(local).toLocalFile();
    }
    const QFileInfo info(local);
    return info.isDir() ? QDir::cleanPath(info.canonicalFilePath()) : QString();
}

QString ProjectWorkspaceRegistry::registeredPath(const QString &path) const
{
    QString local = path.trimmed();
    if (local.startsWith(QLatin1String("file:"), Qt::CaseInsensitive)) {
        local = QUrl(local).toLocalFile();
    }
    if (local.isEmpty()) {
        return {};
    }
    const Project *found = project(locate(local));
    if (!found) {
        // A link or differently spelled path to a folder that exists.
        const QString canonical = canonicalProjectPath(local);
        found = canonical.isEmpty() ? nullptr : project(locate(canonical));
    }
    return found ? found->path : QString();
}

bool ProjectWorkspaceRegistry::contains(const QString &projectPath) const
{
    return locate(projectPath).valid();
}

bool ProjectWorkspaceRegistry::isAvailable(const QString &projectPath) const
{
    const Project *found = project(locate(projectPath));
    return found && found->available;
}

QString ProjectWorkspaceRegistry::parentProject(const QString &projectPath) const
{
    const Project *found = project(locate(projectPath));
    return found ? found->parentPath : QString();
}

QStringList ProjectWorkspaceRegistry::worktreePaths(const QString &projectPath) const
{
    const Location location = locate(projectPath);
    QStringList paths;
    if (location.valid() && location.worktree < 0) {
        for (const Project &worktree : m_projects.at(location.row).worktrees) {
            paths.append(worktree.path);
        }
    }
    return paths;
}

bool ProjectWorkspaceRegistry::isGitRepository(const QString &projectPath) const
{
    const Project *found = project(locate(projectPath));
    return found && found->available && span::git::isWorktreeRoot(found->path);
}

QVariantList ProjectWorkspaceRegistry::unlistedWorktrees(const QString &projectPath) const
{
    const Location location = locate(projectPath);
    if (!location.valid() || location.worktree >= 0) {
        return {};
    }
    QVariantList unlisted;
    for (const span::git::LinkedWorktree &worktree : span::git::linkedWorktreesOf(m_projects.at(location.row).path)) {
        if (contains(worktree.path)) {
            continue;
        }
        unlisted.append(QVariantMap{{QStringLiteral("path"), worktree.path},
                                    {QStringLiteral("name"), folderName(worktree.path)},
                                    {QStringLiteral("branch"), worktree.branch}});
    }
    return unlisted;
}

bool ProjectWorkspaceRegistry::addProjectUrl(const QUrl &url)
{
    return addProject(url.toLocalFile());
}

bool ProjectWorkspaceRegistry::addProject(const QString &path)
{
    const QString canonical = canonicalProjectPath(path);
    if (canonical.isEmpty()) {
        emit errorOccurred(tr("Project path is not a directory: %1").arg(path));
        return false;
    }
    if (const Project *existing = project(locate(canonical))) {
        // Re-opening a registered project refreshes its recency so the next
        // launch orders it first; without this, `order by last_opened` never
        // changed after the initial insert.
        touchLastOpened(existing->path);
        return true;
    }
    Project added;
    added.path = canonical;
    added.name = folderName(canonical);
    added.branch = span::git::headLabelOf(canonical);

    // A linked worktree of a registered top-level project goes under it.
    const QString main = span::git::mainWorktreeOf(canonical);
    const Location parent = main.isEmpty() ? Location{} : locate(main);
    if (parent.valid() && parent.worktree < 0) {
        Project &owner = m_projects[parent.row];
        added.parentPath = owner.path;
        if (!upsert(added)) {
            return false;
        }
        owner.worktrees.append(added);
        sortWorktrees(owner.worktrees);
        if (!owner.expanded) {
            owner.expanded = true;
            upsert(owner);
        }
        notifyRow(parent.row, {ExpandedRole, WorktreesRole});
        emit projectsChanged();
        return true;
    }

    // A new main project adopts its linked worktrees registered at top level.
    QVector<int> adopted;
    for (int row = 0; row < m_projects.size(); ++row) {
        const Project &candidate = m_projects.at(row);
        if (candidate.worktrees.isEmpty() && candidate.available
            && samePath(span::git::mainWorktreeOf(candidate.path), canonical)) {
            adopted.append(row);
        }
    }
    if (!upsert(added)) {
        return false;
    }
    for (auto it = adopted.crbegin(); it != adopted.crend(); ++it) {
        Project worktree = m_projects.at(*it);
        worktree.parentPath = canonical;
        if (!upsert(worktree)) {
            continue;
        }
        added.worktrees.append(worktree);
        beginRemoveRows({}, *it, *it);
        m_projects.removeAt(*it);
        endRemoveRows();
    }
    sortWorktrees(added.worktrees);
    beginInsertRows({}, m_projects.size(), m_projects.size());
    m_projects.append(added);
    endInsertRows();
    emit projectsChanged();
    return true;
}

bool ProjectWorkspaceRegistry::removeProject(const QString &projectPath)
{
    const Location location = locate(projectPath);
    if (!location.valid()) {
        return false;
    }
    if (location.worktree >= 0) {
        Project &owner = m_projects[location.row];
        const QString path = owner.worktrees.at(location.worktree).path;
        if (!deleteRows(path, false)) {
            return false;
        }
        dropContext(path);
        owner.worktrees.removeAt(location.worktree);
        notifyRow(location.row, {WorktreesRole});
        emit projectsChanged();
        return true;
    }
    const Project removed = m_projects.at(location.row);
    if (!deleteRows(removed.path, true)) {
        return false;
    }
    dropContext(removed.path);
    for (const Project &worktree : removed.worktrees) {
        dropContext(worktree.path);
    }
    beginRemoveRows({}, location.row, location.row);
    m_projects.removeAt(location.row);
    endRemoveRows();
    emit projectsChanged();
    return true;
}

bool ProjectWorkspaceRegistry::setExpanded(const QString &projectPath, bool expanded)
{
    const Location location = locate(projectPath);
    Project *found = project(location);
    if (!found || found->expanded == expanded) {
        return found != nullptr;
    }
    Project updated = *found;
    updated.expanded = expanded;
    if (!upsert(updated)) {
        return false;
    }
    found->expanded = expanded;
    notifyRow(location.row, {location.worktree < 0 ? ExpandedRole : WorktreesRole});
    return true;
}

void ProjectWorkspaceRegistry::refreshFolders()
{
    // Whether reading the folder again changed `project`.
    const auto reread = [](Project &project) {
        const bool available = QFileInfo(project.path).isDir();
        const QString branch = span::git::headLabelOf(project.path);
        const bool changed = project.available != available || project.branch != branch;
        project.available = available;
        project.branch = branch;
        return changed;
    };
    for (int row = 0; row < m_projects.size(); ++row) {
        Project &owner = m_projects[row];
        QList<int> roles;
        if (reread(owner)) {
            roles << AvailableRole << BranchRole;
        }
        for (Project &worktree : owner.worktrees) {
            if (reread(worktree) && !roles.contains(WorktreesRole)) {
                roles.append(WorktreesRole);
            }
        }
        if (!roles.isEmpty()) {
            notifyRow(row, roles);
        }
    }
}

ProjectContext *ProjectWorkspaceRegistry::contextForProject(const QString &projectPath)
{
    const Project *found = project(locate(projectPath));
    if (!found) {
        return nullptr;
    }
    const QString canonical = found->path;
    if (auto context = m_contexts.value(canonical)) {
        return context;
    }
    auto *context = new ProjectContext(canonical, m_databasePath, this);
    context->setIdle(m_idle);
    // Before Git is enabled: a background context must not discover yet.
    context->setForeground(m_foregroundProjects.contains(canonical));
    context->setGitEnabled(m_gitEnabled);
    // A checkout in a terminal shows in the project list once Git notices.
    connect(context->gitRepository(), &GitRepositoryService::repositoryChanged, this,
            &ProjectWorkspaceRegistry::refreshFolders);
    m_contexts.insert(canonical, context);
    emit contextCreated(context);
    return context;
}

void ProjectWorkspaceRegistry::setGitEnabled(bool enabled)
{
    if (m_gitEnabled == enabled) {
        return;
    }
    m_gitEnabled = enabled;
    for (const auto &context : m_contexts) {
        if (context) {
            context->setGitEnabled(enabled);
        }
    }
}

void ProjectWorkspaceRegistry::setForegroundProjects(const QStringList &projectPaths)
{
    QSet<QString> foreground;
    for (const QString &path : projectPaths) {
        if (const Project *found = project(locate(path))) {
            foreground.insert(found->path);
        }
    }
    if (foreground == m_foregroundProjects) {
        return;
    }
    m_foregroundProjects = std::move(foreground);
    for (auto it = m_contexts.cbegin(); it != m_contexts.cend(); ++it) {
        if (it.value()) {
            it.value()->setForeground(m_foregroundProjects.contains(it.key()));
        }
    }
}

void ProjectWorkspaceRegistry::setIdle(bool idle)
{
    if (m_idle == idle) {
        return;
    }
    m_idle = idle;
    for (const auto &context : m_contexts) {
        if (context) {
            context->setIdle(idle);
        }
    }
}

ProjectWorkspaceRegistry::Location ProjectWorkspaceRegistry::locate(const QString &projectPath) const
{
    if (projectPath.trimmed().isEmpty()) {
        return {};
    }
    for (int row = 0; row < m_projects.size(); ++row) {
        const Project &owner = m_projects.at(row);
        if (samePath(owner.path, projectPath)) {
            return {row, -1};
        }
        for (int i = 0; i < owner.worktrees.size(); ++i) {
            if (samePath(owner.worktrees.at(i).path, projectPath)) {
                return {row, i};
            }
        }
    }
    return {};
}

const ProjectWorkspaceRegistry::Project *ProjectWorkspaceRegistry::project(const Location &location) const
{
    if (!location.valid()) {
        return nullptr;
    }
    const Project &owner = m_projects.at(location.row);
    return location.worktree < 0 ? &owner : &owner.worktrees.at(location.worktree);
}

ProjectWorkspaceRegistry::Project *ProjectWorkspaceRegistry::project(const Location &location)
{
    if (!location.valid()) {
        return nullptr;
    }
    Project &owner = m_projects[location.row];
    return location.worktree < 0 ? &owner : &owner.worktrees[location.worktree];
}

void ProjectWorkspaceRegistry::notifyRow(int row, const QList<int> &roles)
{
    emit dataChanged(index(row, 0), index(row, 0), roles);
}

void ProjectWorkspaceRegistry::sortWorktrees(QVector<Project> &worktrees)
{
    std::sort(worktrees.begin(), worktrees.end(),
              [](const Project &a, const Project &b) { return a.name.compare(b.name, Qt::CaseInsensitive) < 0; });
}

void ProjectWorkspaceRegistry::dropContext(const QString &projectPath)
{
    if (auto context = m_contexts.take(projectPath)) {
        context->deleteLater();
    }
    m_foregroundProjects.remove(projectPath);
}

bool ProjectWorkspaceRegistry::load()
{
    QSqlDatabase db = QSqlDatabase::addDatabase(span::core::kSqliteDriver, m_connectionName);
    db.setDatabaseName(m_databasePath);
    if (!db.open()) {
        emit errorOccurred(db.lastError().text());
        return false;
    }
    QSqlQuery query(db);
    if (!query.exec(QStringLiteral("select name,path,expanded,parent_path "
                                   "from projects order by last_opened desc,id asc"))) {
        emit errorOccurred(query.lastError().text());
        return false;
    }
    QVector<Project> worktrees;
    while (query.next()) {
        Project project;
        project.name = query.value(0).toString();
        project.path = query.value(1).toString();
        project.expanded = query.value(2).toBool();
        project.parentPath = query.value(3).toString();
        project.available = QFileInfo(project.path).isDir();
        project.branch = span::git::headLabelOf(project.path);
        (project.parentPath.isEmpty() ? m_projects : worktrees).append(project);
    }
    for (Project &worktree : worktrees) {
        const Location parent = locate(worktree.parentPath);
        if (parent.valid() && parent.worktree < 0) {
            m_projects[parent.row].worktrees.append(worktree);
        } else {
            // Its main project is gone (an older build removed it): listed
            // at top level until the main project is added again.
            worktree.parentPath.clear();
            m_projects.append(worktree);
        }
    }
    for (Project &project : m_projects) {
        sortWorktrees(project.worktrees);
    }
    return true;
}

bool ProjectWorkspaceRegistry::touchLastOpened(const QString &projectPath)
{
    QSqlQuery query(QSqlDatabase::database(m_connectionName));
    query.prepare(QStringLiteral(
        "update projects set last_opened=strftime('%s','now') where path=?"));
    query.addBindValue(projectPath);
    if (!query.exec()) {
        emit errorOccurred(query.lastError().text());
        return false;
    }
    return true;
}

bool ProjectWorkspaceRegistry::deleteRows(const QString &projectPath, bool withWorktrees)
{
    QSqlQuery query(QSqlDatabase::database(m_connectionName));
    query.prepare(withWorktrees ? QStringLiteral("delete from projects where path=? or parent_path=?")
                                : QStringLiteral("delete from projects where path=?"));
    query.addBindValue(projectPath);
    if (withWorktrees) {
        query.addBindValue(projectPath);
    }
    if (!query.exec()) {
        emit errorOccurred(query.lastError().text());
        return false;
    }
    return true;
}

bool ProjectWorkspaceRegistry::upsert(const Project &project)
{
    QSqlQuery query(QSqlDatabase::database(m_connectionName));
    query.prepare(QStringLiteral("insert into projects(path,name,last_opened,expanded,parent_path) "
                                 "values(?,?,strftime('%s','now'),?,?) "
                                 "on conflict(path) do update set name=excluded.name,expanded=excluded.expanded,"
                                 "last_opened=excluded.last_opened,parent_path=excluded.parent_path"));
    query.addBindValue(project.path);
    query.addBindValue(project.name);
    query.addBindValue(project.expanded ? 1 : 0);
    // A null QString binds as SQL NULL; the column is `not null`.
    query.addBindValue(project.parentPath.isEmpty() ? QStringLiteral("") : project.parentPath);
    if (!query.exec()) {
        emit errorOccurred(query.lastError().text());
        return false;
    }
    return true;
}

} // namespace span::application
