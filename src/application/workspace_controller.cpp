#include "application/workspace_controller.h"

#include "application/project_context.h"
#include "application/project_workspace_registry.h"
#include "documents/document_manager.h"
#include "git/git_worktree_commands.h"
#include "git/git_worktree_layout.h"
#include "remote/remote_host_registry.h"
#include "terminal/api/terminal_manager.h"
#include "workspace/session_model.h"

#include <QDir>
#include <QFileInfo>
#include <QRegularExpression>

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

bool WorkspaceController::worktreeBusy() const
{
    return m_worktreeCommands && m_worktreeCommands->busy();
}

void WorkspaceController::setWorktreeCommands(GitWorktreeCommands *commands)
{
    if (m_worktreeCommands) {
        disconnect(m_worktreeCommands, nullptr, this, nullptr);
    }
    m_worktreeCommands = commands;
    if (commands) {
        connect(commands, &GitWorktreeCommands::busyChanged, this, &WorkspaceController::worktreeBusyChanged);
    }
    emit worktreeBusyChanged();
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
        emit errorOccurred(m_projects->registeredPath(projectPath).isEmpty()
                               ? tr("Project is not registered: %1").arg(projectPath)
                               : tr("The project folder no longer exists: %1").arg(projectPath));
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
    // Reached for a folder that is gone too, which canonicalProjectPath()
    // cannot resolve: the registry answers with the stored path.
    const QString project = m_projects->registeredPath(projectPath);
    if (project.isEmpty()) {
        return false;
    }
    m_projects->refreshFolders();
    const QStringList affected = QStringList{project} + m_projects->worktreePaths(project);
    for (const QString &path : affected) {
        if (m_projects->isAvailable(path) && hasProjectWork(path)) {
            emit errorOccurred(tr("Close all project tiles and delete all sessions before removing this project."));
            return false;
        }
    }
    // Nothing can run in a folder that is gone: its tiles and sessions go
    // with the entry instead of blocking it.
    for (const QString &path : affected) {
        if (!m_projects->isAvailable(path)) {
            discardProjectWork(path);
        }
    }
    if (!m_projects->removeProject(project)) {
        return false;
    }
    for (const QString &path : affected) {
        if (m_boundScope == Scope::project(path)) {
            setBoundScope({});
        }
    }
    return true;
}

bool WorkspaceController::hasProjectWork(const QString &projectPath) const
{
    return m_layout->tileCountForProject(projectPath) > 0 || m_sessions->countForProject(projectPath) > 0;
}

void WorkspaceController::discardProjectWork(const QString &projectPath)
{
    for (const SessionRecord &session : m_sessions->records()) {
        if (session.projectPath == projectPath) {
            removeSession(session.id);
        }
    }
    QStringList tiles;
    for (const auto &tile : m_layout->tiles()) {
        if (tile.projectPath == projectPath) {
            tiles.append(tile.id);
        }
    }
    for (const QString &tileId : tiles) {
        m_layout->closeTile(tileId);
    }
}

QString WorkspaceController::worktreeNameProblem(const QString &name) const
{
    if (name.isEmpty() || span::git::isValidBranchName(name)) {
        return {};
    }
    return tr("Not a valid Git branch name. Spaces, \"..\" and the characters ~ ^ : ? * [ are not allowed.");
}

QString WorkspaceController::worktreeLocationProblem(const QString &path) const
{
    const QString trimmed = path.trimmed();
    if (trimmed.isEmpty()) {
        return tr("Choose where to create the worktree.");
    }
    if (!QDir::isAbsolutePath(trimmed)) {
        return tr("Use a full path that starts with a drive letter.");
    }
    const QFileInfo info(trimmed);
    if (info.exists()
        && (!info.isDir()
            || !QDir(trimmed).isEmpty(QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System))) {
        return tr("This folder already exists and is not empty.");
    }
    return {};
}

QString WorkspaceController::worktreeRootProblem(const QString &path) const
{
    const QString trimmed = path.trimmed();
    if (trimmed.isEmpty()) {
        return tr("Choose a folder for new worktrees.");
    }
    if (!QDir::isAbsolutePath(trimmed)) {
        return tr("Use a full path that starts with a drive letter.");
    }
    if (QFileInfo(trimmed).isFile()) {
        return tr("This path is a file, not a folder.");
    }
    return {};
}

QString WorkspaceController::suggestedWorktreeRoot(const QString &projectPath) const
{
    const QString project = m_projects->registeredPath(projectPath);
    const QString parent = m_projects->parentProject(project);
    const QFileInfo main(parent.isEmpty() ? project : parent);
    if (main.filePath().isEmpty()) {
        return {};
    }
    return QDir::toNativeSeparators(QDir(main.path()).filePath(QStringLiteral("worktrees")));
}

QString WorkspaceController::worktreeLocation(const QString &root, const QString &projectPath,
                                              const QString &name) const
{
    const QString project = m_projects->registeredPath(projectPath);
    const QString parent = m_projects->parentProject(project);
    const QString mainName = QFileInfo(parent.isEmpty() ? project : parent).fileName();
    if (root.trimmed().isEmpty() || mainName.isEmpty()) {
        return {};
    }
    const QString folder = worktreeFolderName(name);
    QString location = QDir(QDir::fromNativeSeparators(root.trimmed())).filePath(mainName);
    if (!folder.isEmpty()) {
        location = QDir(location).filePath(folder);
    }
    return QDir::toNativeSeparators(QDir::cleanPath(location));
}

QString WorkspaceController::worktreeFolderName(const QString &name) const
{
    // A branch like `feature/login` becomes one folder, `feature-login`.
    static const QRegularExpression pathCharacters(QStringLiteral("[\\\\/:*?\"<>|\\s]+"));
    QString folder = name.trimmed();
    folder.replace(pathCharacters, QStringLiteral("-"));
    return folder;
}

QString WorkspaceController::folderPathFromUrl(const QUrl &url) const
{
    if (!url.isLocalFile()) {
        return {};
    }
    return QDir::toNativeSeparators(QDir::cleanPath(url.toLocalFile()));
}

void WorkspaceController::requestWorktreeBases(const QString &projectPath)
{
    const QString project = m_projects->registeredPath(projectPath);
    if (!m_worktreeCommands || project.isEmpty()) {
        emit worktreeBasesReady(projectPath, {}, {});
        return;
    }
    QPointer<WorkspaceController> self(this);
    m_worktreeCommands->listBranches(project, [self, projectPath](const QStringList &branches, const QString &current) {
        if (self) {
            emit self->worktreeBasesReady(projectPath, branches, current);
        }
    });
}

bool WorkspaceController::createWorktree(const QString &projectPath, const QString &name, const QString &base,
                                         const QString &location)
{
    const QString project = m_projects->registeredPath(projectPath);
    if (!m_worktreeCommands || project.isEmpty() || !m_projects->isAvailable(project)) {
        emit errorOccurred(tr("The project folder no longer exists: %1").arg(projectPath));
        return false;
    }
    if (name.isEmpty() || !worktreeNameProblem(name).isEmpty() || !worktreeLocationProblem(location).isEmpty()
        || base.trimmed().isEmpty()) {
        return false;
    }
    if (m_worktreeCommands->busy()) {
        emit errorOccurred(tr("Another worktree operation is still running."));
        return false;
    }
    const QString target = QDir::cleanPath(QDir::fromNativeSeparators(location.trimmed()));
    QPointer<WorkspaceController> self(this);
    const bool started = m_worktreeCommands->add(
        project, name, base.trimmed(), target, [self, name, target](bool ok, const QString &error) {
            if (!self) {
                return;
            }
            if (!ok) {
                emit self->errorOccurred(tr("Could not create worktree “%1”: %2").arg(name, error));
                return;
            }
            // Listed under its main project (the registry reads the link)
            // and opened in a new terminal.
            self->openProjectFolder(target, true);
        });
    if (!started) {
        emit errorOccurred(tr("Could not start Git to create worktree “%1”.").arg(name));
    }
    return started;
}

bool WorkspaceController::deleteWorktree(const QString &projectPath, bool force)
{
    const QString project = m_projects->registeredPath(projectPath);
    const QString main = m_projects->parentProject(project);
    if (project.isEmpty() || main.isEmpty() || !m_worktreeCommands) {
        return false;
    }
    if (hasProjectWork(project)) {
        emit errorOccurred(tr("Close all tiles and delete all sessions of this worktree before deleting it."));
        return false;
    }
    m_projects->refreshFolders();
    if (!m_projects->isAvailable(project)) {
        // Already gone from disk: only the entry is left.
        return removeProject(project);
    }
    if (m_worktreeCommands->busy()) {
        emit errorOccurred(tr("Another worktree operation is still running."));
        return false;
    }
    QPointer<WorkspaceController> self(this);
    const bool started =
        m_worktreeCommands->remove(main, project, force, [self, project](bool ok, const QString &error) {
            if (!self) {
                return;
            }
            if (!ok) {
                emit self->worktreeDeleteFailed(project, error);
                return;
            }
            self->m_projects->refreshFolders();
            self->removeProject(project);
        });
    if (!started) {
        emit errorOccurred(tr("Could not start Git to delete the worktree."));
    }
    return started;
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
