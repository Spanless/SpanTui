#pragma once

#include "workspace/scope.h"
#include "workspace/workspace_layout.h"

#include <QObject>
#include <QPointer>
#include <QUrl>

class DocumentManager;
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

public:
    WorkspaceController(ProjectWorkspaceRegistry *projects, span::workspace::WorkspaceLayout *layout,
                        SessionModel *sessions, span::terminal::TerminalManager *terminals,
                        span::remote::RemoteHostRegistry *remotes = nullptr, QObject *parent = nullptr);

    QString boundTerminalProjectPath() const;
    QString boundRemoteHostId() const;
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
    Q_INVOKABLE bool removeProject(const QString &projectPath);
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

private:
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
    QString m_pendingEditorTileId;
    QPointer<DocumentManager> m_pendingDocuments;
    span::workspace::Scope m_boundScope;
    QStringList m_foregroundProjects;
};

} // namespace span::application
