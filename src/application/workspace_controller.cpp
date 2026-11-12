#include "application/workspace_controller.h"

#include "application/project_context.h"
#include "application/project_workspace_registry.h"
#include "documents/document_manager.h"
#include "remote/remote_host_registry.h"
#include "terminal/api/terminal_manager.h"
#include "workspace/session_model.h"

#include <QDir>
#include <QFileInfo>

namespace span::application {

using span::workspace::Scope;

WorkspaceController::WorkspaceController(ProjectWorkspaceRegistry *projects, span::workspace::WorkspaceLayout *layout,
                                         SessionModel *sessions, span::terminal::TerminalManager *terminals,
                                         span::remote::RemoteHostRegistry *remotes, QObject *parent)
    : QObject(parent),
      m_projects(projects),
      m_layout(layout),
      m_sessions(sessions),
      m_terminals(terminals),
      m_remotes(remotes)
{
    if (m_layout) {
        connect(m_layout, &span::workspace::WorkspaceLayout::focusedTileIdChanged,
                this, &WorkspaceController::syncBoundTerminalProject);
        connect(m_layout, &span::workspace::WorkspaceLayout::tilesChanged, this,
                &WorkspaceController::syncForegroundProjects);
    }
    connect(m_projects, &ProjectWorkspaceRegistry::projectsChanged, this, &WorkspaceController::syncForegroundProjects);
    connect(this, &WorkspaceController::boundTerminalScopeChanged, this, &WorkspaceController::syncForegroundProjects);
    syncForegroundProjects();
}

QString WorkspaceController::boundTerminalProjectPath() const
{
    return m_boundScope.projectPath();
}

QString WorkspaceController::boundRemoteHostId() const
{
    return m_boundScope.hostId();
}

QStringList WorkspaceController::foregroundProjectPaths() const
{
    return m_foregroundProjects;
}

QString WorkspaceController::showProjectManager()
{
    const QString tileId = m_layout->showProjectsTile();
    m_layout->setSideCollapsed(QStringLiteral("left"), false);
    return tileId;
}

bool WorkspaceController::toggleProjectSidebar()
{
    if (!m_layout->leftCollapsed()) {
        return m_layout->setSideCollapsed(QStringLiteral("left"), true);
    }
    return !showProjectManager().isEmpty();
}

QString WorkspaceController::createProjectTile(const QString &kind,
                                               const QString &projectPath)
{
    const QString canonical = m_projects->canonicalProjectPath(projectPath);
    if (canonical.isEmpty() || !m_projects->contains(canonical)) {
        emit errorOccurred(tr("Project is not registered: %1").arg(projectPath));
        return {};
    }
    return m_layout->addProjectTile(kind, canonical);
}

QString WorkspaceController::createSession(const QString &projectPath,
                                           const QString &name,
                                           const QString &cwd,
                                           const QString &shellProfile)
{
    const QString canonical = m_projects->canonicalProjectPath(projectPath);
    if (canonical.isEmpty() || !m_projects->contains(canonical)) {
        emit errorOccurred(tr("Project is not registered: %1").arg(projectPath));
        return {};
    }
    const QString sessionId = m_sessions->addSession(
        name, cwd.trimmed().isEmpty() ? canonical : cwd, canonical, shellProfile);
    if (sessionId.isEmpty()) {
        return {};
    }
    return showSession(sessionId);
}

QString WorkspaceController::showSession(const QString &sessionId)
{
    const Scope scope = knownScope(m_sessions->projectPathForSession(sessionId));
    if (scope.isEmpty()) {
        return {};
    }
    QString tileId = m_layout->tileIdForSession(sessionId);
    if (tileId.isEmpty()) {
        tileId = m_layout->addProjectTile(QStringLiteral("terminal"), scope.key());
        if (tileId.isEmpty()
            || !m_layout->setTileSessionId(tileId, sessionId)) {
            if (!tileId.isEmpty()) {
                m_layout->closeTile(tileId);
            }
            return {};
        }
    }
    focusTile(tileId);
    return tileId;
}

bool WorkspaceController::focusTile(const QString &tileId)
{
    const auto tile = m_layout->tile(tileId);
    if (!tile) {
        return false;
    }
    m_layout->focusTile(tileId);
    if (tile->kind == QLatin1String("terminal")) {
        m_sessions->activateById(tile->sessionId);
    }
    syncBoundTerminalProject();
    return true;
}

bool WorkspaceController::requestCloseTile(const QString &tileId)
{
    const auto tile = m_layout->tile(tileId);
    if (!tile) {
        return false;
    }
    if (tile->kind != QLatin1String("editor")) {
        return m_layout->closeTile(tileId);
    }
    m_pendingEditorTileId = tileId;
    m_pendingDocuments = existingDocumentsForTile(*tile);
    return continueEditorClose();
}

bool WorkspaceController::closeTerminalTile(const QString &tileId,
                                            bool terminateSession)
{
    const auto tile = m_layout->tile(tileId);
    if (!tile || tile->kind != QLatin1String("terminal") || tile->sessionId.isEmpty()) {
        return false;
    }
    const QString sessionId = tile->sessionId;
    if (!terminateSession) {
        const bool closed = m_layout->closeTile(tileId);
        if (closed) {
            const QString nextFocused = m_layout->focusedTileId();
            if (!nextFocused.isEmpty()) {
                focusTile(nextFocused);
            } else {
                syncBoundTerminalProject();
            }
        }
        return closed;
    }
    // removeSession owns the order: tile -> catalog -> TerminalManager.  The
    // latter stops the ConPTY job before deleting its session object.
    return removeSession(sessionId);
}

bool WorkspaceController::removeSession(const QString &sessionId)
{
    if (m_sessions->indexForSessionId(sessionId) < 0) {
        return false;
    }
    const QString tileId = m_layout->tileIdForSession(sessionId);
    if (!tileId.isEmpty()) {
        m_layout->closeTile(tileId);
    }
    m_sessions->removeSessionById(sessionId);
    m_terminals->closeSession(sessionId);
    const QString nextFocused = m_layout->focusedTileId();
    if (!nextFocused.isEmpty()) {
        focusTile(nextFocused);
    } else {
        syncBoundTerminalProject();
    }
    return true;
}

bool WorkspaceController::removeProject(const QString &projectPath)
{
    const QString canonical = m_projects->canonicalProjectPath(projectPath);
    if (canonical.isEmpty() || !m_projects->contains(canonical)) {
        return false;
    }
    if (m_layout->tileCountForProject(canonical) > 0
        || m_sessions->countForProject(canonical) > 0) {
        emit errorOccurred(tr("Close all project tiles and delete all sessions before removing this project."));
        return false;
    }
    if (!m_projects->removeProject(canonical)) {
        return false;
    }
    if (m_boundScope == Scope::project(canonical)) {
        setBoundScope({});
    }
    return true;
}

QString WorkspaceController::openProjectFolder(const QString &path, bool newTerminal)
{
    // The registry reports a folder that cannot be a project.
    if (!m_projects->addProject(path)) {
        return {};
    }
    const QString canonical = m_projects->canonicalProjectPath(path);
    const int row = newTerminal ? -1 : m_sessions->activeIndexForProject(canonical);
    if (row >= 0) {
        const QString tileId = showSession(m_sessions->sessionIdAt(row));
        if (!tileId.isEmpty()) {
            return tileId;
        }
    }
    return createSession(canonical);
}

QString WorkspaceController::createRemoteSession(const QString &hostId)
{
    if (!m_remotes || !m_remotes->contains(hostId)) {
        emit errorOccurred(tr("The remote host is no longer saved."));
        return {};
    }
    const QString sessionId =
        m_sessions->addSession(m_remotes->hostName(hostId), QString(), Scope::remoteHost(hostId).key());
    if (sessionId.isEmpty()) {
        return {};
    }
    return showSession(sessionId);
}

bool WorkspaceController::removeRemoteHost(const QString &hostId)
{
    if (!m_remotes || !m_remotes->contains(hostId)) {
        return false;
    }
    // The host goes first: when it cannot be deleted, its terminals stay.
    if (!m_remotes->removeHost(hostId)) {
        return false;
    }
    const Scope scope = Scope::remoteHost(hostId);
    for (const SessionRecord &session : m_sessions->records()) {
        if (session.projectPath == scope.key()) {
            removeSession(session.id);
        }
    }
    if (m_boundScope == scope) {
        setBoundScope({});
    }
    return true;
}

span::remote::RemoteHostContext *WorkspaceController::remoteContext(const QString &hostId) const
{
    return m_remotes ? m_remotes->contextForHost(hostId) : nullptr;
}

span::remote::RemoteHostContext *WorkspaceController::remoteContextForScope(const QString &scope) const
{
    const Scope parsed = Scope::fromKey(scope);
    return parsed.isRemoteHost() ? remoteContext(parsed.hostId()) : nullptr;
}

QString WorkspaceController::openFile(const QString &projectPath,
                                      const QString &filePath,
                                      int line,
                                      int column)
{
    ProjectContext *context = projectContext(projectPath);
    if (!context) {
        return {};
    }
    const QString resolvedPath = QFileInfo(filePath).isRelative()
        ? QDir(context->projectPath()).filePath(filePath)
        : filePath;
    const QFileInfo info(resolvedPath);
    const QString canonicalFile = info.canonicalFilePath();
    const QString relative = QDir(context->projectPath())
                                 .relativeFilePath(canonicalFile);
    if (canonicalFile.isEmpty() || relative == QLatin1String("..")
        || relative.startsWith(QStringLiteral("../"))
        || relative.startsWith(QStringLiteral("..\\"))) {
        emit errorOccurred(tr("File is outside the project: %1").arg(filePath));
        return {};
    }
    QString tileId = context->editorTileForFile(canonicalFile);
    if (tileId.isEmpty()) {
        tileId = m_layout->tileIdForKindAndProject(QStringLiteral("editor"),
                                                   context->projectPath());
    }
    if (tileId.isEmpty()) {
        tileId = m_layout->addProjectTile(QStringLiteral("editor"),
                                          context->projectPath());
    }
    DocumentManager *documents = context->editorDocuments(tileId);
    if (!documents || !documents->openFileAt(canonicalFile, line, column)) {
        return {};
    }
    m_layout->focusTile(tileId);
    return tileId;
}

QString WorkspaceController::openFileUrl(const QString &projectPath, const QUrl &fileUrl)
{
    const QString path = fileUrl.isLocalFile() ? fileUrl.toLocalFile() : QString();
    if (path.isEmpty() || !QFileInfo(path).isFile()) {
        emit errorOccurred(tr("Linked file not found: %1").arg(fileUrl.toDisplayString()));
        return {};
    }
    return openFile(projectPath, path);
}

ProjectContext *WorkspaceController::projectContext(const QString &projectPath) const
{
    return m_projects->contextForProject(projectPath);
}

bool WorkspaceController::continueEditorClose()
{
    if (!m_pendingDocuments) {
        const QString tileId = m_pendingEditorTileId;
        m_pendingEditorTileId.clear();
        return m_layout->closeTile(tileId);
    }
    while (m_pendingDocuments->rowCount() > 0) {
        if (!m_pendingDocuments->closeDocument(0)) {
            return false;
        }
    }
    const auto tile = m_layout->tile(m_pendingEditorTileId);
    ProjectContext *context = projectContext(tile ? tile->projectPath : QString());
    const QString tileId = m_pendingEditorTileId;
    if (context) {
        context->releaseEditor(tileId);
    }
    m_pendingEditorTileId.clear();
    m_pendingDocuments.clear();
    return m_layout->closeTile(tileId);
}

void WorkspaceController::syncBoundTerminalProject()
{
    const auto focused = m_layout->tile(m_layout->focusedTileId());
    Scope candidate;
    if (focused && focused->kind == QLatin1String("terminal")) {
        candidate = knownScope(focused->projectPath);
    } else if (m_boundScope.isEmpty()) {
        for (const auto &tile : m_layout->tiles()) {
            if (tile.kind != QLatin1String("terminal")) {
                continue;
            }
            candidate = knownScope(tile.projectPath);
            if (!candidate.isEmpty()) {
                break;
            }
        }
    } else {
        return;
    }
    if (!candidate.isEmpty()) {
        setBoundScope(candidate);
    }
}

void WorkspaceController::syncForegroundProjects()
{
    QStringList projects;
    const auto add = [this, &projects](const QString &scopeKey) {
        const QString canonical = knownScope(scopeKey).projectPath();
        if (!canonical.isEmpty() && !projects.contains(canonical)) {
            projects.append(canonical);
        }
    };
    if (m_layout) {
        for (const auto &tile : m_layout->tiles()) {
            add(tile.projectPath);
        }
    }
    add(m_boundScope.key());
    if (projects == m_foregroundProjects) {
        return;
    }
    m_foregroundProjects = projects;
    m_projects->setForegroundProjects(m_foregroundProjects);
}

Scope WorkspaceController::knownScope(const QString &scopeKey) const
{
    const Scope scope = Scope::fromKey(scopeKey);
    if (scope.isRemoteHost()) {
        return m_remotes && m_remotes->contains(scope.hostId()) ? scope : Scope();
    }
    const QString canonical = m_projects->canonicalProjectPath(scope.projectPath());
    return !canonical.isEmpty() && m_projects->contains(canonical) ? Scope::project(canonical) : Scope();
}

void WorkspaceController::setBoundScope(const Scope &scope)
{
    if (m_boundScope == scope) {
        return;
    }
    m_boundScope = scope;
    emit boundTerminalScopeChanged();
}

DocumentManager *WorkspaceController::existingDocumentsForTile(const span::workspace::WorkspaceLayout::Tile &tile) const
{
    ProjectContext *context = projectContext(tile.projectPath);
    return context ? context->existingEditorDocuments(tile.id) : nullptr;
}

} // namespace span::application
