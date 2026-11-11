#include "application/preferences.h"
#include "application/session_terminal_routing.h"
#include "application/shell_profile_registry.h"
#include "remote/remote_host_registry.h"
#include "terminal/api/terminal_manager.h"
#include "terminal/api/terminal_session.h"
#include "workspace/scope.h"
#include "workspace/session_model.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTest>

#include <memory>

using span::application::ShellProfileRegistry;
using span::remote::RemoteHostRegistry;
using span::terminal::TerminalManager;

namespace {

QString projectPath(const QTemporaryDir &dir, const QString &name)
{
    const QString path = dir.filePath(name);
    QDir().mkpath(path);
    return QFileInfo(path).canonicalFilePath();
}

QString cmd()
{
    return QStandardPaths::findExecutable(QStringLiteral("cmd.exe"));
}

// cmd.exe arguments that write `marker` and exit; it starts in a fraction of
// PowerShell's time.
QString writeMarkerArguments(const QString &marker)
{
    return QStringLiteral("/d /c echo started>\"%1\"").arg(QDir::toNativeSeparators(marker));
}

// The production routing (application/session_terminal_routing.h) between a
// real catalog, terminal manager, host registry and shell profiles, declared
// in the runtime's member order.
struct Workspace {
    explicit Workspace(const QTemporaryDir &dir)
    {
        const QString databasePath = dir.filePath(QStringLiteral("state.db"));
        state.store.setSessionModel(&sessions);
        state.store.setDatabasePathForTesting(databasePath);
        initialized = state.open(span::application::SettingsStore::Mode::Workspace);
        shells = std::make_unique<ShellProfileRegistry>(&state.terminal);
        remoteHosts = std::make_unique<RemoteHostRegistry>(databasePath, nullptr);
        span::application::connectSessionTerminals(&sessions, &terminals, remoteHosts.get(),
                                                   shells.get());
    }

    SessionModel sessions;
    TerminalManager terminals;
    span::application::Preferences state;
    bool initialized = false;
    std::unique_ptr<ShellProfileRegistry> shells;
    std::unique_ptr<RemoteHostRegistry> remoteHosts;
};

} // namespace

class WorkspaceSessionRoutingTest final : public QObject {
    Q_OBJECT

private slots:
    void initTestCase();
    void activationDrivesTerminalManager();
    void commandSessionCommandIsConsumedOnce();
    void removingCatalogSessionDropsItsLiveProcess();
    void remoteSessionOfUnknownHostIsNotLaunched();
    void shellProfileReachesOnlyNewTerminals();
};

void WorkspaceSessionRoutingTest::initTestCase()
{
    QStandardPaths::setTestModeEnabled(true);
}

void WorkspaceSessionRoutingTest::activationDrivesTerminalManager()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString projectA = projectPath(dir, QStringLiteral("alpha"));
    const QString projectB = projectPath(dir, QStringLiteral("beta"));

    Workspace workspace(dir);
    QVERIFY(workspace.initialized);
    SessionModel &sessions = workspace.sessions;
    TerminalManager &manager = workspace.terminals;
    sessions.addSession(QStringLiteral("alpha"), projectA, projectA);
    const QString idA = sessions.activeSessionId();
    sessions.addSession(QStringLiteral("beta"), projectB, projectB);
    const QString idB = sessions.activeSessionId();

    QVERIFY(!idA.isEmpty());
    QVERIFY(!idB.isEmpty());
    QVERIFY(idA != idB);
    QTRY_VERIFY(manager.session(idA));
    QTRY_VERIFY(manager.session(idB));
    QTRY_VERIFY(manager.session(idA)->running());
    QTRY_VERIFY(manager.session(idB)->running());
    QCOMPARE(manager.activeSession(), manager.session(idB));

    const int alphaIndex = sessions.indexForSessionId(idA);
    QVERIFY(alphaIndex >= 0);
    sessions.activate(alphaIndex);
    QCOMPARE(manager.activeSession(), manager.session(idA));
    QVERIFY(manager.session(idB)->running());
}

void WorkspaceSessionRoutingTest::commandSessionCommandIsConsumedOnce()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString project = projectPath(dir, QStringLiteral("run"));
    const QString marker = dir.filePath(QStringLiteral("command-started.txt"));
    const QString command = QStringLiteral("cmd.exe ") + writeMarkerArguments(marker);

    Workspace workspace(dir);
    QVERIFY(workspace.initialized);
    SessionModel &sessions = workspace.sessions;
    sessions.addCommandSession(QStringLiteral("run"), project, project, command);
    const QString commandId = sessions.activeSessionId();
    QVERIFY(!commandId.isEmpty());
    QCOMPARE(sessions.takeInitialCommandLine(commandId), QString());
    QTRY_VERIFY_WITH_TIMEOUT(QFileInfo::exists(marker), 10000);
    QVERIFY(QFile::remove(marker));

    const int row = sessions.indexForSessionId(commandId);
    QVERIFY(row >= 0);
    sessions.renameSession(row, QStringLiteral("renamed run"));
    QVERIFY(!QFileInfo::exists(marker));
    QVERIFY(sessions.takeInitialCommandLine(commandId).isEmpty());
}

void WorkspaceSessionRoutingTest::removingCatalogSessionDropsItsLiveProcess()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString project = projectPath(dir, QStringLiteral("project"));
    Workspace workspace(dir);
    QVERIFY(workspace.initialized);
    SessionModel &sessions = workspace.sessions;
    TerminalManager &manager = workspace.terminals;
    sessions.addSession(QStringLiteral("shell"), project, project);
    const QString id = sessions.activeSessionId();
    QTRY_VERIFY(manager.session(id));
    QTRY_VERIFY(manager.session(id)->running());

    const int row = sessions.indexForSessionId(id);
    QVERIFY(row >= 0);
    sessions.removeSession(row);
    QTRY_VERIFY(manager.session(id) == nullptr);
    QVERIFY(sessions.activeSessionId().isEmpty());
}

void WorkspaceSessionRoutingTest::remoteSessionOfUnknownHostIsNotLaunched()
{
    // A remote session survives in the catalog when the host list could not
    // be read; without its ssh command it would otherwise open a local shell.
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString project = projectPath(dir, QStringLiteral("local"));
    Workspace workspace(dir);
    QVERIFY(workspace.initialized);
    const QString scope = span::workspace::Scope::remoteHost(QStringLiteral("missing-host")).key();
    const QString remoteId =
        workspace.sessions.addSession(QStringLiteral("remote"), QString(), scope);
    QVERIFY(!remoteId.isEmpty());
    const QString localId = workspace.sessions.addSession(QStringLiteral("local"), project, project);

    QTRY_VERIFY(workspace.terminals.session(localId));
    QVERIFY(workspace.terminals.session(remoteId) == nullptr);
}

void WorkspaceSessionRoutingTest::shellProfileReachesOnlyNewTerminals()
{
    if (cmd().isEmpty()) QSKIP("cmd.exe is not available");
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString project = projectPath(dir, QStringLiteral("profiles"));
    const QString first = dir.filePath(QStringLiteral("first.txt"));
    const QString edited = dir.filePath(QStringLiteral("edited.txt"));

    Workspace workspace(dir);
    QVERIFY(workspace.initialized);
    ShellProfileRegistry &shells = *workspace.shells;
    const QString profile = shells.addCustomProfile(cmd());
    QVERIFY(!profile.isEmpty());
    shells.setArguments(profile, writeMarkerArguments(first));

    SessionModel &sessions = workspace.sessions;
    const QString id = sessions.addSession(QStringLiteral("pinned"), project, project, profile);
    QTRY_VERIFY_WITH_TIMEOUT(QFileInfo::exists(first), 10000);

    // Editing the profile and resynchronizing keeps the existing terminal on
    // the command it was created with; a new session uses the edit.
    shells.setArguments(profile, writeMarkerArguments(edited));
    sessions.renameSession(sessions.indexForSessionId(id), QStringLiteral("renamed"));
    QVERIFY(workspace.terminals.session(id));
    // A restarted cmd.exe would write the marker within this window.
    QTest::qWait(250);
    QVERIFY(!QFileInfo::exists(edited));

    sessions.addSession(QStringLiteral("fresh"), project, project, profile);
    QTRY_VERIFY_WITH_TIMEOUT(QFileInfo::exists(edited), 10000);
}

QTEST_MAIN(WorkspaceSessionRoutingTest)
#include "workspace_session_routing_test.moc"
