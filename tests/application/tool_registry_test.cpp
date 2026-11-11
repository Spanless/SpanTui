#include "application/preferences.h"
#include "application/tool_registry.h"
#include "core/external_tools.h"

#include <QDir>
#include <QFileInfo>
#include <QScopeGuard>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QtTest>

using span::application::Preferences;
using span::application::ToolRegistry;
using span::core::ExternalTool;

class ToolRegistryTest final : public QObject {
    Q_OBJECT

private slots:
    void parsesProbeVersions();
    void enforcesGitMinimumVersion();
    void invalidOverrideDisablesToolWithoutFallback();
    void unrunnableExecutableIsReportedBroken();
    void realGitBecomesAvailable();
    void clearingOverrideReturnsToDetection();

private:
    bool initState(Preferences &state, QTemporaryDir &dir);
    static int rowFor(const ToolRegistry &tools, const QString &toolId);
    static QString role(const ToolRegistry &tools, const QString &toolId, int role);
};

bool ToolRegistryTest::initState(Preferences &state, QTemporaryDir &dir)
{
    if (!dir.isValid())
        return false;
    state.store.setDatabasePathForTesting(dir.filePath(QStringLiteral("state.db")));
    return state.open(span::application::SettingsStore::Mode::Workspace);
}

int ToolRegistryTest::rowFor(const ToolRegistry &tools, const QString &toolId)
{
    for (int row = 0; row < tools.rowCount(); ++row) {
        if (tools.data(tools.index(row), ToolRegistry::ToolIdRole).toString() == toolId)
            return row;
    }
    return -1;
}

QString ToolRegistryTest::role(const ToolRegistry &tools, const QString &toolId, int role)
{
    return tools.data(tools.index(rowFor(tools, toolId)), role).toString();
}

void ToolRegistryTest::parsesProbeVersions()
{
    QCOMPARE(ToolRegistry::parseVersion(ExternalTool::Git, "git version 2.45.1.windows.1\n"),
             QStringLiteral("2.45.1.windows.1"));
    QCOMPARE(ToolRegistry::parseVersion(ExternalTool::Git, "git version 2.39.0\n"),
             QStringLiteral("2.39.0"));
    QCOMPARE(ToolRegistry::parseVersion(ExternalTool::Ssh,
                                        "OpenSSH_for_Windows_9.5p1, LibreSSL 3.8.2\r\n"),
             QStringLiteral("9.5p1"));
    QCOMPARE(ToolRegistry::parseVersion(ExternalTool::Ssh, "OpenSSH_8.1p1, OpenSSL 1.1.1"),
             QStringLiteral("8.1p1"));
    QCOMPARE(ToolRegistry::parseVersion(ExternalTool::Git, "Microsoft Windows [Version 10.0]"),
             QString());
    QCOMPARE(ToolRegistry::parseVersion(ExternalTool::Tailscale,
                                        "1.104.2\r\n  tailscale commit: 0123abc\r\n"
                                        "  go version: go1.25.1\r\n"),
             QStringLiteral("1.104.2"));
    QCOMPARE(ToolRegistry::parseVersion(ExternalTool::Tailscale, "failed to connect to local tailscaled\r\n"),
             QString());
}

void ToolRegistryTest::enforcesGitMinimumVersion()
{
    QVERIFY(ToolRegistry::meetsMinimum(ExternalTool::Git, QStringLiteral("2.16.0")));
    QVERIFY(ToolRegistry::meetsMinimum(ExternalTool::Git, QStringLiteral("2.45.1.windows.1")));
    QVERIFY(!ToolRegistry::meetsMinimum(ExternalTool::Git, QStringLiteral("2.13.3")));
    QVERIFY(!ToolRegistry::meetsMinimum(ExternalTool::Git, QString()));
    // ssh has no minimum.
    QVERIFY(ToolRegistry::meetsMinimum(ExternalTool::Ssh, QString()));
}

void ToolRegistryTest::invalidOverrideDisablesToolWithoutFallback()
{
    QTemporaryDir dir;
    Preferences state;
    QVERIFY(initState(state, dir));
    ToolRegistry tools(&state.workspace);
    tools.refresh();

    const QString missing = dir.filePath(QStringLiteral("no-such-git.exe"));
    QSignalSpy executable(&tools, &ToolRegistry::executableChanged);
    tools.setOverridePath(QStringLiteral("git"), missing);

    QCOMPARE(tools.status(ExternalTool::Git), ToolRegistry::Status::InvalidOverride);
    QVERIFY(!tools.gitAvailable());
    QCOMPARE(role(tools, QStringLiteral("git"), ToolRegistry::StatusRole), QStringLiteral("invalid"));
    QCOMPARE(role(tools, QStringLiteral("git"), ToolRegistry::OverrideRole),
             QDir::toNativeSeparators(missing));
    // An explicit but broken choice never silently falls back to detection.
    QCOMPARE(span::core::toolExecutable(ExternalTool::Git), QString());
    QCOMPARE(state.workspace.toolPathOverrides().value(QStringLiteral("git")).toString(),
             QDir::toNativeSeparators(missing));

    // Relative paths would resolve against the opened folder: rejected too.
    tools.setOverridePath(QStringLiteral("git"), QStringLiteral("git.exe"));
    QCOMPARE(tools.status(ExternalTool::Git), ToolRegistry::Status::InvalidOverride);
}

void ToolRegistryTest::unrunnableExecutableIsReportedBroken()
{
    // argv_echo_child runs and exits 0 but prints no Git version. It records
    // its arguments in the probe's working directory (the test TEMP).
    const QString child =
        QDir(QCoreApplication::applicationDirPath()).filePath(QStringLiteral("argv_echo_child.exe"));
    QVERIFY(QFileInfo::exists(child));
    const auto removeRecord = qScopeGuard(
        []() { QFile::remove(QDir::temp().filePath(QStringLiteral("argv.txt"))); });
    QTemporaryDir dir;
    Preferences state;
    QVERIFY(initState(state, dir));
    ToolRegistry tools(&state.workspace);
    tools.setOverridePath(QStringLiteral("git"), QDir::toNativeSeparators(child));

    QTRY_VERIFY_WITH_TIMEOUT(tools.status(ExternalTool::Git) != ToolRegistry::Status::Checking,
                             10000);
    QCOMPARE(tools.status(ExternalTool::Git), ToolRegistry::Status::Broken);
    QCOMPARE(role(tools, QStringLiteral("git"), ToolRegistry::StatusRole), QStringLiteral("broken"));
    QVERIFY(!tools.gitAvailable());
    QCOMPARE(span::core::toolExecutable(ExternalTool::Git), QString());
}

void ToolRegistryTest::realGitBecomesAvailable()
{
    const QString git = span::core::defaultToolExecutable(ExternalTool::Git);
    if (git.isEmpty())
        QSKIP("Git is not installed");
    QTemporaryDir dir;
    Preferences state;
    QVERIFY(initState(state, dir));
    ToolRegistry tools(&state.workspace);
    QSignalSpy checking(&tools, &ToolRegistry::checkingChanged);
    tools.refresh();

    // Found counts as available while the probe runs.
    QVERIFY(tools.gitAvailable());
    QCOMPARE(span::core::toolExecutable(ExternalTool::Git), git);
    QCOMPARE(role(tools, QStringLiteral("git"), ToolRegistry::StatusRole), QStringLiteral("checking"));
    QTRY_VERIFY_WITH_TIMEOUT(!tools.checking(), 10000);
    QCOMPARE(tools.status(ExternalTool::Git), ToolRegistry::Status::Available);
    QCOMPARE(role(tools, QStringLiteral("git"), ToolRegistry::StatusRole), QStringLiteral("available"));
    QCOMPARE(role(tools, QStringLiteral("git"), ToolRegistry::MinimumVersionRole),
             QStringLiteral("2.16"));
    QVERIFY(role(tools, QStringLiteral("ssh"), ToolRegistry::MinimumVersionRole).isEmpty());
    QVERIFY(!role(tools, QStringLiteral("git"), ToolRegistry::VersionRole).isEmpty());
    QCOMPARE(role(tools, QStringLiteral("git"), ToolRegistry::PathRole), git);
    QVERIFY(checking.count() >= 2);
}

void ToolRegistryTest::clearingOverrideReturnsToDetection()
{
    QTemporaryDir dir;
    Preferences state;
    QVERIFY(initState(state, dir));
    ToolRegistry tools(&state.workspace);
    tools.setOverridePath(QStringLiteral("ssh"), dir.filePath(QStringLiteral("missing.exe")));
    QCOMPARE(tools.status(ExternalTool::Ssh), ToolRegistry::Status::InvalidOverride);

    tools.setOverridePath(QStringLiteral("ssh"), QString());
    QVERIFY(!state.workspace.toolPathOverrides().contains(QStringLiteral("ssh")));
    const QString detected = span::core::defaultToolExecutable(ExternalTool::Ssh);
    QCOMPARE(role(tools, QStringLiteral("ssh"), ToolRegistry::PathRole), detected);
    QCOMPARE(tools.status(ExternalTool::Ssh) == ToolRegistry::Status::Missing, detected.isEmpty());
}

QTEST_MAIN(ToolRegistryTest)
#include "tool_registry_test.moc"
