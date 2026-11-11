#include "application/project_workspace_registry.h"

#include "application/project_context.h"
#include "core/core_constants.h"

#include <QDir>
#include <QUrl>
#include <QFileInfo>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QUuid>

namespace span::application {

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
    default: return {};
    }
}

QHash<int, QByteArray> ProjectWorkspaceRegistry::roleNames() const
{
    return {{NameRole, "name"}, {PathRole, "path"}, {ExpandedRole, "expanded"}};
}

int ProjectWorkspaceRegistry::count() const { return m_projects.size(); }
bool ProjectWorkspaceRegistry::needsInitialProject() const { return m_projects.isEmpty(); }

QStringList ProjectWorkspaceRegistry::projectPaths() const
{
    QStringList paths;
    paths.reserve(m_projects.size());
    for (const Project &project : m_projects) {
        paths.append(project.path);
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

bool ProjectWorkspaceRegistry::contains(const QString &projectPath) const
{
    return indexForPath(projectPath) >= 0;
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
    if (indexForPath(canonical) >= 0) {
        // Re-opening a registered project refreshes its recency so the next
        // launch orders it first; without this, `order by last_opened` never
        // changed after the initial insert.
        touchLastOpened(canonical);
        return true;
    }
    const QFileInfo info(canonical);
    Project project;
    project.path = canonical;
    project.name = info.fileName().isEmpty() ? canonical : info.fileName();
    if (!upsert(project)) {
        return false;
    }
    beginInsertRows({}, m_projects.size(), m_projects.size());
    m_projects.append(project);
    endInsertRows();
    emit projectsChanged();
    return true;
}

bool ProjectWorkspaceRegistry::removeProject(const QString &projectPath)
{
    const int row = indexForPath(projectPath);
    if (row < 0) {
        return false;
    }
    QSqlDatabase db = QSqlDatabase::database(m_connectionName);
    QSqlQuery query(db);
    query.prepare(QStringLiteral("delete from projects where path=?"));
    query.addBindValue(m_projects.at(row).path);
    if (!query.exec()) {
        emit errorOccurred(query.lastError().text());
        return false;
    }
    if (auto context = m_contexts.take(m_projects.at(row).path)) {
        context->deleteLater();
    }
    beginRemoveRows({}, row, row);
    m_projects.removeAt(row);
    endRemoveRows();
    emit projectsChanged();
    return true;
}

bool ProjectWorkspaceRegistry::setExpanded(const QString &projectPath, bool expanded)
{
    const int row = indexForPath(projectPath);
    if (row < 0 || m_projects.at(row).expanded == expanded) {
        return row >= 0;
    }
    Project updated = m_projects.at(row);
    updated.expanded = expanded;
    if (!upsert(updated)) {
        return false;
    }
    m_projects[row] = updated;
    emit dataChanged(index(row, 0), index(row, 0), {ExpandedRole});
    return true;
}

ProjectContext *ProjectWorkspaceRegistry::contextForProject(const QString &projectPath)
{
    const int row = indexForPath(projectPath);
    if (row < 0) {
        return nullptr;
    }
    const QString canonical = m_projects.at(row).path;
    if (auto context = m_contexts.value(canonical)) {
        return context;
    }
    auto *context = new ProjectContext(canonical, m_databasePath, this);
    context->setIdle(m_idle);
    // Before Git is enabled: a background context must not discover yet.
    context->setForeground(m_foregroundProjects.contains(canonical));
    context->setGitEnabled(m_gitEnabled);
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
        const int row = indexForPath(path);
        if (row >= 0) {
            foreground.insert(m_projects.at(row).path);
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

int ProjectWorkspaceRegistry::indexForPath(const QString &projectPath) const
{
    const QString clean = QDir::cleanPath(projectPath);
    for (int i = 0; i < m_projects.size(); ++i) {
        if (QDir::cleanPath(m_projects.at(i).path).compare(clean, Qt::CaseInsensitive) == 0) {
            return i;
        }
    }
    return -1;
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
    if (!query.exec(QStringLiteral(
            "select name,path,expanded "
            "from projects order by last_opened desc,id asc"))) {
        emit errorOccurred(query.lastError().text());
        return false;
    }
    while (query.next()) {
        Project project;
        project.name = query.value(0).toString();
        project.path = query.value(1).toString();
        project.expanded = query.value(2).toBool();
        m_projects.append(project);
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

bool ProjectWorkspaceRegistry::upsert(const Project &project)
{
    QSqlQuery query(QSqlDatabase::database(m_connectionName));
    query.prepare(QStringLiteral(
        "insert into projects(path,name,last_opened,expanded) "
        "values(?,?,strftime('%s','now'),?) "
        "on conflict(path) do update set name=excluded.name,expanded=excluded.expanded,"
        "last_opened=excluded.last_opened"));
    query.addBindValue(project.path);
    query.addBindValue(project.name);
    query.addBindValue(project.expanded ? 1 : 0);
    if (!query.exec()) {
        emit errorOccurred(query.lastError().text());
        return false;
    }
    return true;
}

} // namespace span::application
