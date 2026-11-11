#include "application/project_context.h"
#include "application/project_workspace_registry.h"
#include "application/settings_store.h"
#include "application/workspace_controller.h"
#include "git/git_repository_service.h"
#include "remote/remote_host_registry.h"
#include "terminal/api/terminal_manager.h"
#include "workspace/session_model.h"
#include "workspace/workspace_layout.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>

using span::application::ProjectContext;
using span::application::ProjectWorkspaceRegistry;
using span::application::WorkspaceController;
using span::remote::RemoteHostRegistry;
using span::terminal::TerminalManager;
using span::workspace::WorkspaceLayout;

class WorkspaceControllerTest final : public QObject {
    Q_OBJECT

private slots:
    void addingProjectDoesNotCreateSession();
    void createSessionAddsCatalogAndTerminalTile();
    void closingTerminalTileWithoutTerminationKeepsSession();
    void closingTerminalTileWithTerminationRemovesSessionAndProcessOwner();
    void removeSessionClosesTileAndCatalog();
    void removeProjectRefusedWhileSessionExists();
    void removeProjectRefusedWhileTileExists();
    void showProjectManagerReusesSingleton();
    void toggleProjectSidebarCollapsesLeft();
    void boundTerminalProjectFollowsFocusedTerminal();
    void createProjectTileRejectsRetiredKinds();
    void openFileReusesExistingEditorTile();
    void openFileUrlOpensLinkedFileInEditor();
    void remoteSessionBindsHostScope();
    void onlyShownProjectsAreForeground();
};

namespace {

QString makeProject(const QTemporaryDir &dir, const QString &name)
{
    const QString path = dir.filePath(name);
    QDir().mkpath(path);
    return QFileInfo(path).canonicalFilePath();
}

void writeBytes(const QString &path, const QByteArray &bytes)
{
    QFile file(path);
    QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
    QCOMPARE(file.write(bytes), bytes.size());
}

void prepareSchema(const QString &databasePath)
{
    span::application::SettingsStore state;
    state.setDatabasePathForTesting(databasePath);
    QVERIFY(state.open());
}

QString tileKind(const WorkspaceLayout &layout, const QString &tileId)
{
    return layout.tileById(tileId).value(QStringLiteral("kind")).toString();
}

} // namespace

void WorkspaceControllerTest::addingProjectDoesNotCreateSession()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString databasePath = dir.filePath(QStringLiteral("state.db"));
    const QString project = makeProject(dir, QStringLiteral("alpha"));
    prepareSchema(databasePath);

    ProjectWorkspaceRegistry projects(databasePath);
    WorkspaceLayout layout;
    SessionModel sessions;
    TerminalManager terminals;
    WorkspaceController controller(&projects, &layout, &sessions, &terminals);

    QVERIFY(projects.addProject(project));
    QCOMPARE(sessions.count(), 0);
    QCOMPARE(layout.tileCountForProject(project), 0);
}

void WorkspaceControllerTest::createSessionAddsCatalogAndTerminalTile()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString databasePath = dir.filePath(QStringLiteral("state.db"));
    const QString project = makeProject(dir, QStringLiteral("alpha"));
    prepareSchema(databasePath);

    ProjectWorkspaceRegistry projects(databasePath);
    WorkspaceLayout layout;
    SessionModel sessions;
    TerminalManager terminals;
    WorkspaceController controller(&projects, &layout, &sessions, &terminals);
    QVERIFY(projects.addProject(project));

    const QString tileId = controller.createSession(project);
    QVERIFY(!tileId.isEmpty());
    QCOMPARE(sessions.count(), 1);
    QCOMPARE(sessions.countForProject(project), 1);
    QCOMPARE(tileKind(layout, tileId), QStringLiteral("terminal"));
    QCOMPARE(layout.tileIdForSession(sessions.activeSessionId()), tileId);
}

void WorkspaceControllerTest::closingTerminalTileWithoutTerminationKeepsSession()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString databasePath = dir.filePath(QStringLiteral("state.db"));
    const QString project = makeProject(dir, QStringLiteral("alpha"));
    prepareSchema(databasePath);

    ProjectWorkspaceRegistry projects(databasePath);
    WorkspaceLayout layout;
    SessionModel sessions;
    TerminalManager terminals;
    WorkspaceController controller(&projects, &layout, &sessions, &terminals);
    QVERIFY(projects.addProject(project));
    const QString tileId = controller.createSession(project);
    const QString sessionId = sessions.activeSessionId();
    QVERIFY(!sessionId.isEmpty());
    QVERIFY(terminals.createSession(sessionId, project) != nullptr);

    QVERIFY(controller.closeTerminalTile(tileId, false));
    QCOMPARE(sessions.count(), 1);
    QCOMPARE(sessions.activeSessionId(), sessionId);
    QVERIFY(layout.tileIdForSession(sessionId).isEmpty());
    QVERIFY(terminals.session(sessionId) != nullptr);

    const QString reopened = controller.showSession(sessionId);
    QVERIFY(!reopened.isEmpty());
    QCOMPARE(layout.tileIdForSession(sessionId), reopened);
}

void WorkspaceControllerTest::closingTerminalTileWithTerminationRemovesSessionAndProcessOwner()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString databasePath = dir.filePath(QStringLiteral("state.db"));
    const QString project = makeProject(dir, QStringLiteral("alpha"));
    prepareSchema(databasePath);

    ProjectWorkspaceRegistry projects(databasePath);
    WorkspaceLayout layout;
    SessionModel sessions;
    TerminalManager terminals;
    WorkspaceController controller(&projects, &layout, &sessions, &terminals);
    QVERIFY(projects.addProject(project));
    const QString retainedTileId = controller.createSession(project);
    const QString retainedSessionId = sessions.activeSessionId();
    const QString tileId = controller.createSession(project);
    const QString sessionId = sessions.activeSessionId();
    QVERIFY(terminals.createSession(retainedSessionId, project) != nullptr);
    QVERIFY(terminals.createSession(sessionId, project) != nullptr);

    QVERIFY(controller.closeTerminalTile(tileId, true));
    QCOMPARE(sessions.count(), 1);
    QVERIFY(layout.tileById(tileId).isEmpty());
    QVERIFY(terminals.session(sessionId) == nullptr);
    QCOMPARE(sessions.activeSessionId(), retainedSessionId);
    QCOMPARE(layout.tileIdForSession(retainedSessionId), retainedTileId);
}

void WorkspaceControllerTest::removeSessionClosesTileAndCatalog()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString databasePath = dir.filePath(QStringLiteral("state.db"));
    const QString project = makeProject(dir, QStringLiteral("alpha"));
    prepareSchema(databasePath);

    ProjectWorkspaceRegistry projects(databasePath);
    WorkspaceLayout layout;
    SessionModel sessions;
    TerminalManager terminals;
    WorkspaceController controller(&projects, &layout, &sessions, &terminals);
    QVERIFY(projects.addProject(project));
    const QString tileId = controller.createSession(project);
    const QString sessionId = sessions.activeSessionId();

    QVERIFY(controller.removeSession(sessionId));
    QCOMPARE(sessions.count(), 0);
    QVERIFY(layout.tileById(tileId).isEmpty());
}

void WorkspaceControllerTest::removeProjectRefusedWhileSessionExists()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString databasePath = dir.filePath(QStringLiteral("state.db"));
    const QString project = makeProject(dir, QStringLiteral("alpha"));
    prepareSchema(databasePath);

    ProjectWorkspaceRegistry projects(databasePath);
    WorkspaceLayout layout;
    SessionModel sessions;
    TerminalManager terminals;
    WorkspaceController controller(&projects, &layout, &sessions, &terminals);
    QVERIFY(projects.addProject(project));
    QVERIFY(!controller.createSession(project).isEmpty());

    QSignalSpy errors(&controller, &WorkspaceController::errorOccurred);
    QVERIFY(!controller.removeProject(project));
    QVERIFY(projects.contains(project));
    QCOMPARE(errors.size(), 1);

    QVERIFY(controller.removeSession(sessions.activeSessionId()));
    QVERIFY(controller.removeProject(project));
    QVERIFY(!projects.contains(project));
}

void WorkspaceControllerTest::removeProjectRefusedWhileTileExists()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString databasePath = dir.filePath(QStringLiteral("state.db"));
    const QString project = makeProject(dir, QStringLiteral("alpha"));
    prepareSchema(databasePath);

    ProjectWorkspaceRegistry projects(databasePath);
    WorkspaceLayout layout;
    SessionModel sessions;
    TerminalManager terminals;
    WorkspaceController controller(&projects, &layout, &sessions, &terminals);
    QVERIFY(projects.addProject(project));
    const QString editor = controller.createProjectTile(QStringLiteral("editor"), project);
    QVERIFY(!editor.isEmpty());

    QVERIFY(!controller.removeProject(project));
    QVERIFY(projects.contains(project));
    QVERIFY(controller.requestCloseTile(editor));
    QVERIFY(controller.removeProject(project));
}

void WorkspaceControllerTest::showProjectManagerReusesSingleton()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString databasePath = dir.filePath(QStringLiteral("state.db"));
    prepareSchema(databasePath);

    ProjectWorkspaceRegistry projects(databasePath);
    WorkspaceLayout layout;
    SessionModel sessions;
    TerminalManager terminals;
    WorkspaceController controller(&projects, &layout, &sessions, &terminals);

    const QString first = controller.showProjectManager();
    const QString second = controller.showProjectManager();
    QCOMPARE(first, second);
    QCOMPARE(tileKind(layout, first), QStringLiteral("projects"));
}

void WorkspaceControllerTest::toggleProjectSidebarCollapsesLeft()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString databasePath = dir.filePath(QStringLiteral("state.db"));
    prepareSchema(databasePath);

    ProjectWorkspaceRegistry projects(databasePath);
    WorkspaceLayout layout;
    SessionModel sessions;
    TerminalManager terminals;
    WorkspaceController controller(&projects, &layout, &sessions, &terminals);

    QVERIFY(!layout.leftCollapsed());
    QVERIFY(controller.toggleProjectSidebar());
    QVERIFY(layout.leftCollapsed());
    QVERIFY(controller.toggleProjectSidebar());
    QVERIFY(!layout.leftCollapsed());
}

void WorkspaceControllerTest::boundTerminalProjectFollowsFocusedTerminal()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString databasePath = dir.filePath(QStringLiteral("state.db"));
    const QString project = makeProject(dir, QStringLiteral("alpha"));
    prepareSchema(databasePath);

    ProjectWorkspaceRegistry projects(databasePath);
    WorkspaceLayout layout;
    SessionModel sessions;
    TerminalManager terminals;
    WorkspaceController controller(&projects, &layout, &sessions, &terminals);
    QVERIFY(projects.addProject(project));
    QCOMPARE(controller.boundTerminalProjectPath(), QString());
    QVERIFY(!controller.createSession(project).isEmpty());
    QCOMPARE(controller.boundTerminalProjectPath(), project);
    const QString editor = controller.createProjectTile(QStringLiteral("editor"), project);
    QVERIFY(controller.focusTile(editor));
    QCOMPARE(controller.boundTerminalProjectPath(), project);

    const QString json = layout.toJson();
    WorkspaceLayout restored;
    WorkspaceController restoredController(&projects, &restored, &sessions, &terminals);
    QCOMPARE(restoredController.boundTerminalProjectPath(), QString());
    QVERIFY(restored.restoreFromJson(json));
    QCOMPARE(restoredController.boundTerminalProjectPath(), project);
}

void WorkspaceControllerTest::createProjectTileRejectsRetiredKinds()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString databasePath = dir.filePath(QStringLiteral("state.db"));
    const QString project = makeProject(dir, QStringLiteral("alpha"));
    prepareSchema(databasePath);

    ProjectWorkspaceRegistry projects(databasePath);
    WorkspaceLayout layout;
    SessionModel sessions;
    TerminalManager terminals;
    WorkspaceController controller(&projects, &layout, &sessions, &terminals);
    QVERIFY(projects.addProject(project));
    QCOMPARE(controller.createProjectTile(QStringLiteral("files"), project), QString());
    QCOMPARE(controller.createProjectTile(QStringLiteral("gitCommit"), project), QString());
    QCOMPARE(controller.createProjectTile(QStringLiteral("diff"), project), QString());
}

void WorkspaceControllerTest::openFileReusesExistingEditorTile()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString databasePath = dir.filePath(QStringLiteral("state.db"));
    const QString project = makeProject(dir, QStringLiteral("alpha"));
    const QString filePath = QDir(project).filePath(QStringLiteral("notes.txt"));
    writeBytes(filePath, "hello\n");
    prepareSchema(databasePath);

    ProjectWorkspaceRegistry projects(databasePath);
    WorkspaceLayout layout;
    SessionModel sessions;
    TerminalManager terminals;
    WorkspaceController controller(&projects, &layout, &sessions, &terminals);
    QVERIFY(projects.addProject(project));

    const QString first = controller.openFile(project, filePath);
    QVERIFY(!first.isEmpty());
    QCOMPARE(tileKind(layout, first), QStringLiteral("editor"));
    const QString second = controller.openFile(project, filePath);
    QCOMPARE(second, first);
    const QString relative = controller.openFile(project, QStringLiteral("notes.txt"));
    QCOMPARE(relative, first);
}

void WorkspaceControllerTest::openFileUrlOpensLinkedFileInEditor()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString databasePath = dir.filePath(QStringLiteral("state.db"));
    const QString project = makeProject(dir, QStringLiteral("alpha"));
    const QString script = QDir(project).filePath(QStringLiteral("tool run.bat"));
    writeBytes(script, "@echo off\n");
    prepareSchema(databasePath);

    ProjectWorkspaceRegistry projects(databasePath);
    WorkspaceLayout layout;
    SessionModel sessions;
    TerminalManager terminals;
    WorkspaceController controller(&projects, &layout, &sessions, &terminals);
    QVERIFY(projects.addProject(project));
    QSignalSpy errors(&controller, &WorkspaceController::errorOccurred);

    // A Markdown link to a script opens its text in the editor.
    const QString tile = controller.openFileUrl(project, QUrl::fromLocalFile(script));
    QVERIFY(!tile.isEmpty());
    QCOMPARE(tileKind(layout, tile), QStringLiteral("editor"));
    QCOMPARE(errors.count(), 0);

    QVERIFY(controller.openFileUrl(project, QUrl::fromLocalFile(project)).isEmpty());
    QVERIFY(controller.openFileUrl(
                project, QUrl::fromLocalFile(QDir(project).filePath(QStringLiteral("missing.md"))))
                .isEmpty());
    QVERIFY(controller.openFileUrl(project, QUrl(QStringLiteral("https://example.com/a.md")))
                .isEmpty());
    QCOMPARE(errors.count(), 3);
}

void WorkspaceControllerTest::remoteSessionBindsHostScope()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString databasePath = dir.filePath(QStringLiteral("state.db"));
    const QString project = makeProject(dir, QStringLiteral("alpha"));
    prepareSchema(databasePath);

    ProjectWorkspaceRegistry projects(databasePath);
    RemoteHostRegistry remotes(databasePath, nullptr);
    WorkspaceLayout layout;
    SessionModel sessions;
    TerminalManager terminals;
    WorkspaceController controller(&projects, &layout, &sessions, &terminals, &remotes);
    QVERIFY(projects.addProject(project));
    const QString hostId = remotes.saveHost({{QStringLiteral("host"), QStringLiteral("example.com")},
                                             {QStringLiteral("name"), QStringLiteral("web")}});
    QVERIFY(!hostId.isEmpty());

    QSignalSpy errors(&controller, &WorkspaceController::errorOccurred);
    QVERIFY(controller.createRemoteSession(QStringLiteral("missing")).isEmpty());
    QCOMPARE(errors.size(), 1);

    QVERIFY(!controller.createSession(project).isEmpty());
    QCOMPARE(controller.boundTerminalProjectPath(), project);
    QSignalSpy scope(&controller, &WorkspaceController::boundTerminalScopeChanged);
    const QString remoteTile = controller.createRemoteSession(hostId);
    QVERIFY(!remoteTile.isEmpty());
    QCOMPARE(tileKind(layout, remoteTile), QStringLiteral("terminal"));
    QCOMPARE(sessions.nameForSessionId(sessions.activeSessionId()), QStringLiteral("web"));
    // A remote terminal binds the host; the local project binding clears.
    QCOMPARE(controller.boundRemoteHostId(), hostId);
    QCOMPARE(controller.boundTerminalProjectPath(), QString());
    QVERIFY(scope.size() >= 1);
    QVERIFY(controller.remoteContext(hostId) != nullptr);
    QCOMPARE(controller.remoteContext(hostId), controller.remoteContext(hostId));
    QCOMPARE(controller.remoteContextForScope(QStringLiteral("remote:") + hostId), controller.remoteContext(hostId));
    QCOMPARE(controller.remoteContextForScope(project), nullptr);

    QVERIFY(controller.removeRemoteHost(hostId));
    QVERIFY(sessions.sessionsForRemoteHost(hostId).isEmpty());
    QVERIFY(layout.tileById(remoteTile).isEmpty());
    QCOMPARE(remotes.count(), 0);
    QVERIFY(controller.boundRemoteHostId().isEmpty());
}

void WorkspaceControllerTest::onlyShownProjectsAreForeground()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString databasePath = dir.filePath(QStringLiteral("state.db"));
    const QString alpha = makeProject(dir, QStringLiteral("alpha"));
    const QString beta = makeProject(dir, QStringLiteral("beta"));
    prepareSchema(databasePath);

    ProjectWorkspaceRegistry projects(databasePath);
    // Foreground is about polling; no Git process is needed to observe it.
    projects.setGitEnabled(false);
    WorkspaceLayout layout;
    SessionModel sessions;
    TerminalManager terminals;
    WorkspaceController controller(&projects, &layout, &sessions, &terminals);
    QVERIFY(projects.addProject(alpha));
    QVERIFY(projects.addProject(beta));
    const auto git = [&projects](const QString &project) {
        return projects.contextForProject(project)->gitRepository();
    };
    QVERIFY(controller.foregroundProjectPaths().isEmpty());
    // A context nothing shows starts in the background.
    QVERIFY(!git(alpha)->foreground());

    const QString alphaTile = controller.createProjectTile(QStringLiteral("editor"), alpha);
    QVERIFY(!alphaTile.isEmpty());
    QCOMPARE(controller.foregroundProjectPaths(), QStringList{alpha});
    QVERIFY(git(alpha)->foreground());
    QVERIFY(!git(beta)->foreground());

    const QString betaTile = controller.createProjectTile(QStringLiteral("editor"), beta);
    QCOMPARE(controller.foregroundProjectPaths(), (QStringList{alpha, beta}));
    QVERIFY(git(beta)->foreground());

    // Closing the last tile of a project sends it to the background.
    QVERIFY(controller.requestCloseTile(alphaTile));
    QCOMPARE(controller.foregroundProjectPaths(), QStringList{beta});
    QVERIFY(!git(alpha)->foreground());

    // The right dock's project stays foreground without a tile of its own.
    const QString terminalTile = controller.createSession(alpha);
    QVERIFY(!terminalTile.isEmpty());
    QCOMPARE(controller.boundTerminalProjectPath(), alpha);
    QVERIFY(controller.closeTerminalTile(terminalTile, false));
    QCOMPARE(controller.boundTerminalProjectPath(), alpha);
    QVERIFY(git(alpha)->foreground());
    QVERIFY(git(beta)->foreground());
}

QTEST_MAIN(WorkspaceControllerTest)
#include "workspace_controller_test.moc"
