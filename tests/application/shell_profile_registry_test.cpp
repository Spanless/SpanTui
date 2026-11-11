#include "application/preferences.h"
#include "application/shell_profile_registry.h"
#include "terminal/api/shell_catalog.h"

#include <QDir>
#include <QFile>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QtTest>

#include <algorithm>

using span::application::Preferences;
using span::application::ShellProfileRegistry;

// Ids of the shells whose arguments include `argument`; every id when it is empty.
static QStringList shellIdsPassing(const QVector<span::terminal::ShellCandidate> &shells, const QString &argument)
{
    QStringList ids;
    for (const auto &shell : shells) {
        if (argument.isEmpty() || shell.arguments.contains(argument)) ids.append(shell.id);
    }
    return ids;
}

class ShellProfileRegistryTest final : public QObject {
    Q_OBJECT

private slots:
    void commandLineQuotesExecutable();
    void detectsSystemShells();
    void gitBashFollowsGitExecutable();
    void defaultFallsBackToFirstDetected();
    void detectedArgumentsStoreOnlyDifferences();
    void customProfilesCanBeChosenAndRemoved();
    void missingCustomProgramIsNotStartable();

private:
    bool initState(Preferences &state, QTemporaryDir &dir);
    static bool touch(const QString &path);
    static QStringList launchIds(const ShellProfileRegistry &shells);
};

bool ShellProfileRegistryTest::initState(Preferences &state, QTemporaryDir &dir)
{
    if (!dir.isValid())
        return false;
    state.store.setDatabasePathForTesting(dir.filePath(QStringLiteral("state.db")));
    return state.open(span::application::SettingsStore::Mode::Workspace);
}

bool ShellProfileRegistryTest::touch(const QString &path)
{
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile file(path);
    return file.open(QIODevice::WriteOnly);
}

QStringList ShellProfileRegistryTest::launchIds(const ShellProfileRegistry &shells)
{
    QStringList ids;
    for (const QVariant &profile : shells.launchProfiles())
        ids << profile.toMap().value(QStringLiteral("profileId")).toString();
    return ids;
}

void ShellProfileRegistryTest::commandLineQuotesExecutable()
{
    QCOMPARE(span::terminal::shellCommandLine(QStringLiteral("C:/Program Files/PowerShell/7/pwsh.exe"),
                                              QStringLiteral(" -NoLogo ")),
             QStringLiteral("\"C:\\Program Files\\PowerShell\\7\\pwsh.exe\" -NoLogo"));
    QCOMPARE(span::terminal::shellCommandLine(QStringLiteral("C:\\Windows\\System32\\cmd.exe"), QString()),
             QStringLiteral("\"C:\\Windows\\System32\\cmd.exe\""));
}

void ShellProfileRegistryTest::detectsSystemShells()
{
    const auto shells = span::terminal::detectShells();
    QVERIFY(!shells.isEmpty());
    const auto cmd = std::find_if(shells.cbegin(), shells.cend(), [](const auto &shell) {
        return shell.id == QStringLiteral("cmd");
    });
    QVERIFY(cmd != shells.cend());
    QVERIFY(QFileInfo(cmd->executable).isFile());
    QCOMPARE(cmd->icon, cmd->executable);
    // The old -NoProfile default is gone: PowerShell loads the user's profile.
    QCOMPARE(shellIdsPassing(shells, QStringLiteral("-NoProfile")), QStringList{});
    // Git Bash needs a Git executable.
    QVERIFY(!shellIdsPassing(shells, {}).contains(QStringLiteral("git-bash")));
}

void ShellProfileRegistryTest::gitBashFollowsGitExecutable()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString git = dir.filePath(QStringLiteral("Git/cmd/git.exe"));
    const QString bash = dir.filePath(QStringLiteral("Git/bin/bash.exe"));
    const QString launcher = dir.filePath(QStringLiteral("Git/git-bash.exe"));
    QVERIFY(touch(git));
    QVERIFY(touch(bash));
    QVERIFY(touch(launcher));

    const auto shells = span::terminal::detectShells(git);
    const auto found = std::find_if(shells.cbegin(), shells.cend(), [](const auto &shell) {
        return shell.id == QStringLiteral("git-bash");
    });
    QVERIFY(found != shells.cend());
    QCOMPARE(found->executable, QDir::toNativeSeparators(bash));
    QCOMPARE(found->arguments, QStringLiteral("--login -i"));
    // bash.exe has no icon; Git's launcher next to it has.
    QCOMPARE(found->icon, QDir::toNativeSeparators(launcher));
}

void ShellProfileRegistryTest::defaultFallsBackToFirstDetected()
{
    QTemporaryDir dir;
    Preferences state;
    QVERIFY(initState(state, dir));
    ShellProfileRegistry shells(&state.terminal);
    const QStringList ids = launchIds(shells);
    QVERIFY(ids.contains(QStringLiteral("cmd")));
    QCOMPARE(shells.defaultProfileId(), ids.first());

    QSignalSpy defaultChanged(&shells, &ShellProfileRegistry::defaultProfileChanged);
    shells.setDefaultProfile(QStringLiteral("cmd"));
    QCOMPARE(shells.defaultProfileId(), QStringLiteral("cmd"));
    QCOMPARE(state.terminal.shellProfiles().value(QStringLiteral("default")).toString(), QStringLiteral("cmd"));
    QCOMPARE(defaultChanged.count(), ids.first() == QStringLiteral("cmd") ? 0 : 1);

    const QString cmdLine = shells.commandLine(QStringLiteral("cmd"));
    QVERIFY(cmdLine.endsWith(QStringLiteral("cmd.exe\"")));
    // Sessions without a profile, or with one that no longer exists, start
    // the default.
    QCOMPARE(shells.commandLine(QString()), cmdLine);
    QCOMPARE(shells.commandLine(QStringLiteral("wsl:NoSuchDistribution")), cmdLine);

    // An id that names no profile is not stored.
    shells.setDefaultProfile(QStringLiteral("nothing"));
    QCOMPARE(shells.defaultProfileId(), QStringLiteral("cmd"));
}

void ShellProfileRegistryTest::detectedArgumentsStoreOnlyDifferences()
{
    QTemporaryDir dir;
    Preferences state;
    QVERIFY(initState(state, dir));
    ShellProfileRegistry shells(&state.terminal);

    shells.setArguments(QStringLiteral("cmd"), QStringLiteral(" /k echo ready "));
    QVERIFY(shells.commandLine(QStringLiteral("cmd")).endsWith(QStringLiteral("cmd.exe\" /k echo ready")));
    QCOMPARE(state.terminal.shellProfiles().value(QStringLiteral("arguments")).toMap(),
             (QVariantMap{{QStringLiteral("cmd"), QStringLiteral("/k echo ready")}}));

    // Back to the detected default: nothing is stored.
    shells.setArguments(QStringLiteral("cmd"), QString());
    QVERIFY(!state.terminal.shellProfiles().contains(QStringLiteral("arguments")));
    QVERIFY(shells.commandLine(QStringLiteral("cmd")).endsWith(QStringLiteral("cmd.exe\"")));
}

void ShellProfileRegistryTest::customProfilesCanBeChosenAndRemoved()
{
    QTemporaryDir dir;
    Preferences state;
    QVERIFY(initState(state, dir));
    ShellProfileRegistry shells(&state.terminal);
    const QString detectedDefault = shells.defaultProfileId();
    const QString program = dir.filePath(QStringLiteral("tools/nu.exe"));
    QVERIFY(touch(program));

    const QString id = shells.addCustomProfileUrl(QUrl::fromLocalFile(program));
    QVERIFY(id.startsWith(QStringLiteral("custom:")));
    QVERIFY(launchIds(shells).contains(id));
    const int row = int(launchIds(shells).size()) - 1;
    QCOMPARE(shells.data(shells.index(row), ShellProfileRegistry::NameRole).toString(),
             QStringLiteral("nu"));
    QVERIFY(shells.data(shells.index(row), ShellProfileRegistry::CustomRole).toBool());

    shells.setArguments(id, QStringLiteral("--login"));
    shells.setDefaultProfile(id);
    QCOMPARE(shells.defaultProfileId(), id);
    QCOMPARE(shells.commandLine(QString()),
             QStringLiteral("\"%1\" --login").arg(QDir::toNativeSeparators(program)));

    // A second registry reads the same stored choices.
    ShellProfileRegistry reread(&state.terminal);
    QCOMPARE(reread.defaultProfileId(), id);

    shells.removeCustomProfile(id);
    QVERIFY(!launchIds(shells).contains(id));
    QCOMPARE(shells.defaultProfileId(), detectedDefault);
    QVERIFY(!state.terminal.shellProfiles().contains(QStringLiteral("default")));
}

void ShellProfileRegistryTest::missingCustomProgramIsNotStartable()
{
    QTemporaryDir dir;
    Preferences state;
    QVERIFY(initState(state, dir));
    ShellProfileRegistry shells(&state.terminal);
    const QString id = shells.addCustomProfile(dir.filePath(QStringLiteral("gone.exe")));
    QVERIFY(!id.isEmpty());
    QCOMPARE(shells.rowCount(), int(launchIds(shells).size()) + 1);
    QVERIFY(!launchIds(shells).contains(id));

    shells.setDefaultProfile(id);
    QVERIFY(shells.defaultProfileId() != id);
    QCOMPARE(shells.commandLine(id), shells.commandLine(QString()));
}

QTEST_MAIN(ShellProfileRegistryTest)
#include "shell_profile_registry_test.moc"
