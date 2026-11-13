#pragma once

#include "workspace/scope.h"
#include "workspace/workspace_layout.h"

#include <QObject>
#include <QPointer>
#include <QUrl>

class DocumentManager;
class GitWorktreeCommands;
class SessionModel;

namespace span::remote {
class RemoteHostContext;
class RemoteHostRegistry;
}
namespace span::terminal { class TerminalManager; }

namespace span::application {

class ProjectContext;
class ProjectWorkspaceRegistry;

class WorkspaceController final : public QObject {
    Q_OBJECT
    Q_MOC_INCLUDE("application/project_context.h")
    Q_MOC_INCLUDE("remote/remote_host_context.h")
    // The right dock follows the focused terminal's scope: a local project
    // (boundTerminalProjectPath) or a saved SSH host (boundRemoteHostId).
    // At most one of the two is non-empty.
    Q_PROPERTY(QString boundTerminalProjectPath READ boundTerminalProjectPath NOTIFY boundTerminalScopeChanged FINAL)
    Q_PROPERTY(QString boundRemoteHostId READ boundRemoteHostId NOTIFY boundTerminalScopeChanged FINAL)
    // A `git worktree add`/`remove` is running; only one runs at a time.
    Q_PROPERTY(bool worktreeBusy READ worktreeBusy NOTIFY worktreeBusyChanged FINAL)

public:
    WorkspaceController(ProjectWorkspaceRegistry *projects, span::workspace::WorkspaceLayout *layout,
                        SessionModel *sessions, span::terminal::TerminalManager *terminals,
                        span::remote::RemoteHostRegistry *remotes = nullptr, QObject *parent = nullptr);

    QString boundTerminalProjectPath() const;
    QString boundRemoteHostId() const;
    bool worktreeBusy() const;
    // The runner of worktree Git commands; set once by the runtime. Without
    // it the worktree commands fail.
    void setWorktreeCommands(GitWorktreeCommands *commands);
    // Projects that a center tile or the right dock shows, in tile order.
    // Only these poll Git; the set is pushed to the project registry.
    QStringList foregroundProjectPaths() const;

    QString showProjectManager();
    Q_INVOKABLE bool toggleProjectSidebar();
    Q_INVOKABLE QString createProjectTile(const QString &kind,
                                          const QString &projectPath);
    // `shellProfile` "" follows the default shell profile.
    Q_INVOKABLE QString createSession(const QString &projectPath,
                                      const QString &name = {},
                                      const QString &cwd = {},
                                      const QString &shellProfile = {});
    Q_INVOKABLE QString showSession(const QString &sessionId);
    Q_INVOKABLE bool focusTile(const QString &tileId);
    Q_INVOKABLE bool requestCloseTile(const QString &tileId);
    Q_INVOKABLE bool closeTerminalTile(const QString &tileId,
                                       bool terminateSession);
    Q_INVOKABLE bool removeSession(const QString &sessionId);
    // Drops the project from the list (a top-level project takes its
    // worktree entries along). A project whose folder still exists must have
    // no tiles or sessions left; one whose folder is gone loses them here.
    Q_INVOKABLE bool removeProject(const QString &projectPath);

    // Worktrees. A new worktree is created as `<root>\<main project>\<name>`
    // by default, on a new branch `name` from `base`; it is then listed
    // under its main project and opens in a new terminal.
    // Translated reason `name` cannot be a new branch; "" when it can (and
    // for an empty name, which the dialog handles itself).
    Q_INVOKABLE QString worktreeNameProblem(const QString &name) const;
    // Translated reason a worktree cannot be created at `path`; "" when it can.
    Q_INVOKABLE QString worktreeLocationProblem(const QString &path) const;
    // Translated reason `path` cannot be the default worktree folder; "" when
    // it can. The folder need not exist yet.
    Q_INVOKABLE QString worktreeRootProblem(const QString &path) const;
    // First-run suggestion for the worktree root: `worktrees` beside the
    // main project folder.
    Q_INVOKABLE QString suggestedWorktreeRoot(const QString &projectPath) const;
    // `<root>\<main project name>\<worktreeFolderName(name)>`.
    Q_INVOKABLE QString worktreeLocation(const QString &root, const QString &projectPath, const QString &name) const;
    // `name` with path characters replaced, so `feature/login` is one
    // folder, `feature-login`.
    Q_INVOKABLE QString worktreeFolderName(const QString &name) const;
    // A folder picker's URL as a native local path; "" for a non-local URL.
    Q_INVOKABLE QString folderPathFromUrl(const QUrl &url) const;
    // Answers through worktreeBasesReady.
    Q_INVOKABLE void requestWorktreeBases(const QString &projectPath);
    Q_INVOKABLE bool createWorktree(const QString &projectPath, const QString &name, const QString &base,
                                    const QString &location);
    // `git worktree remove` of a worktree project, then drops its entry. Its
    // tiles and sessions must be closed first. Git refuses a worktree with
    // changes unless `force`; that refusal comes back as worktreeDeleteFailed.
    Q_INVOKABLE bool deleteWorktree(const QString &projectPath, bool force = false);
    // Registers a folder as a project (a no-op when it already is one) and
    // shows it: its last focused session, else a new terminal; with
    // `newTerminal`, always a new terminal. Returns the terminal tile id,
    // empty when the folder cannot be a project.
    Q_INVOKABLE QString openProjectFolder(const QString &path, bool newTerminal = false);
    // Opens a new ssh terminal for a saved host.
    Q_INVOKABLE QString createRemoteSession(const QString &hostId);
    // Ends every session of the host, then deletes the saved host.
    Q_INVOKABLE bool removeRemoteHost(const QString &hostId);
    Q_INVOKABLE span::remote::RemoteHostContext *remoteContext(const QString &hostId) const;
    // The host context behind a session/tile scope key; null for a local
    // project path.
    Q_INVOKABLE span::remote::RemoteHostContext *remoteContextForScope(const QString &scope) const;
    Q_INVOKABLE QString openFile(const QString &projectPath,
                                 const QString &filePath,
                                 int line = 1,
                                 int column = 1);
    // Opens a local file:// link (e.g. from a Markdown document) in the
    // project's editor. Never hands the file to the shell, so a link to a
    // script or executable cannot run it.
    Q_INVOKABLE QString openFileUrl(const QString &projectPath, const QUrl &fileUrl);
    Q_INVOKABLE span::application::ProjectContext *projectContext(const QString &projectPath) const;

signals:
    void errorOccurred(const QString &message);
    void boundTerminalScopeChanged();
    void worktreeBusyChanged();
    // Local then remote branches; both empty when the read failed.
    void worktreeBasesReady(const QString &projectPath, const QStringList &branches, const QString &current);
    void worktreeDeleteFailed(const QString &projectPath, const QString &error);

private:
    // Tiles and sessions of a project whose folder is gone.
    void discardProjectWork(const QString &projectPath);
    bool hasProjectWork(const QString &projectPath) const;
    bool continueEditorClose();
    void syncBoundTerminalProject();
    void syncForegroundProjects();
    // Scope of a registered project (canonical path) or saved host; empty
    // otherwise.
    span::workspace::Scope knownScope(const QString &scopeKey) const;
    void setBoundScope(const span::workspace::Scope &scope);
    // The editor tile's document manager; null when none was created yet.
    DocumentManager *existingDocumentsForTile(const span::workspace::WorkspaceLayout::Tile &tile) const;

    ProjectWorkspaceRegistry *m_projects = nullptr;
    span::workspace::WorkspaceLayout *m_layout = nullptr;
    SessionModel *m_sessions = nullptr;
    span::terminal::TerminalManager *m_terminals = nullptr;
    span::remote::RemoteHostRegistry *m_remotes = nullptr;
    QPointer<GitWorktreeCommands> m_worktreeCommands;
    QString m_pendingEditorTileId;
    QPointer<DocumentManager> m_pendingDocuments;
    span::workspace::Scope m_boundScope;
    QStringList m_foregroundProjects;
};

} // namespace span::application
