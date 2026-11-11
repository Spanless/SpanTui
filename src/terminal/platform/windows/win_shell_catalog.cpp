#include "terminal/api/shell_catalog.h"

#include "core/external_tools.h"

#include <QDir>
#include <QFileInfo>
#include <QSettings>

#include <algorithm>

namespace span::terminal {
namespace {

QString underEnvironment(const char *variable, const QString &relative)
{
    const QString root = qEnvironmentVariable(variable);
    return root.isEmpty() ? QString() : QDir(root).filePath(relative);
}

QString existingFile(const QString &path)
{
    return !path.isEmpty() && QFileInfo(path).isFile() ? QDir::toNativeSeparators(path)
                                                       : QString();
}

// The system copy wins: a same-named program earlier on PATH is not the
// shell the profile names.
QString systemOrSearched(const QString &systemPath, const QString &fileName,
                         const QStringList &searchPath)
{
    const QString system = existingFile(systemPath);
    return system.isEmpty() ? core::findExecutableIn(fileName, searchPath) : system;
}

// Git for Windows keeps git.exe in cmd/, bin/ or mingw64/bin/ and bash.exe
// in bin/ of the same installation. Returns the installation root.
QString gitInstallationOf(const QString &gitExecutable)
{
    if (gitExecutable.isEmpty()) {
        return {};
    }
    QDir dir = QFileInfo(gitExecutable).absoluteDir();
    for (int depth = 0; depth < 3; ++depth) {
        if (!existingFile(dir.filePath(QStringLiteral("bin/bash.exe"))).isEmpty()) {
            return dir.absolutePath();
        }
        if (!dir.cdUp()) {
            break;
        }
    }
    return {};
}

// "PowerShell 7" for an install under PowerShell/7/, as Windows Terminal
// names it; plain "PowerShell" when the folder carries no version (the
// Microsoft Store alias).
QString powerShellName(const QString &pwsh)
{
    const QString folder = QFileInfo(pwsh).absoluteDir().dirName();
    qsizetype digits = 0;
    while (digits < folder.size() && folder.at(digits).isDigit()) {
        ++digits;
    }
    return digits > 0 ? QStringLiteral("PowerShell ") + folder.left(digits)
                      : QStringLiteral("PowerShell");
}

QStringList wslDistributions()
{
    const QSettings lxss(
        QStringLiteral("HKEY_CURRENT_USER\\Software\\Microsoft\\Windows\\CurrentVersion\\Lxss"),
        QSettings::NativeFormat);
    QStringList names;
    for (const QString &key : lxss.childGroups()) {
        const QString name =
            lxss.value(key + QStringLiteral("/DistributionName")).toString().trimmed();
        // Docker Desktop's internal distributions are not interactive shells.
        if (!name.isEmpty() && !name.startsWith(QStringLiteral("docker-desktop"))) {
            names << name;
        }
    }
    std::sort(names.begin(), names.end(), [](const QString &a, const QString &b) {
        return a.compare(b, Qt::CaseInsensitive) < 0;
    });
    return names;
}

} // namespace

QVector<ShellCandidate> detectShells(const QString &gitExecutable)
{
    const QStringList searchPath = core::executableSearchPath();
    QVector<ShellCandidate> shells;
    const auto add = [&shells](const QString &id, const QString &name,
                               const QString &executable, const QString &arguments,
                               const QString &icon = {}) {
        if (!executable.isEmpty()) {
            shells.push_back({id, name, executable, icon.isEmpty() ? executable : icon,
                              arguments});
        }
    };

    QString pwsh = core::findExecutableIn(QStringLiteral("pwsh.exe"), searchPath);
    if (pwsh.isEmpty()) {
        pwsh = existingFile(underEnvironment("ProgramW6432", QStringLiteral("PowerShell/7/pwsh.exe")));
    }
    if (pwsh.isEmpty()) {
        pwsh = existingFile(underEnvironment("ProgramFiles", QStringLiteral("PowerShell/7/pwsh.exe")));
    }
    add(QStringLiteral("pwsh"), powerShellName(pwsh), pwsh, QStringLiteral("-NoLogo"));
    add(QStringLiteral("powershell"), QStringLiteral("Windows PowerShell"),
        systemOrSearched(underEnvironment("SystemRoot",
                                          QStringLiteral("System32/WindowsPowerShell/v1.0/powershell.exe")),
                         QStringLiteral("powershell.exe"), searchPath),
        QStringLiteral("-NoLogo"));
    add(QStringLiteral("cmd"), QStringLiteral("Command Prompt"),
        systemOrSearched(underEnvironment("SystemRoot", QStringLiteral("System32/cmd.exe")),
                         QStringLiteral("cmd.exe"), searchPath),
        QString());
    const QString git = gitInstallationOf(gitExecutable);
    if (!git.isEmpty()) {
        const QDir root(git);
        // bash.exe carries no icon of its own; git-bash.exe has Git's.
        add(QStringLiteral("git-bash"), QStringLiteral("Git Bash"),
            existingFile(root.filePath(QStringLiteral("bin/bash.exe"))), QStringLiteral("--login -i"),
            existingFile(root.filePath(QStringLiteral("git-bash.exe"))));
    }

    const QString wsl = existingFile(underEnvironment("SystemRoot", QStringLiteral("System32/wsl.exe")));
    if (!wsl.isEmpty()) {
        for (const QString &distribution : wslDistributions()) {
            add(QStringLiteral("wsl:") + distribution, distribution, wsl,
                QStringLiteral("-d \"%1\"").arg(distribution));
        }
    }
    return shells;
}

QString shellCommandLine(const QString &executable, const QString &arguments)
{
    const QString command =
        QLatin1Char('"') + QDir::toNativeSeparators(executable) + QLatin1Char('"');
    const QString rest = arguments.trimmed();
    return rest.isEmpty() ? command : command + QLatin1Char(' ') + rest;
}

} // namespace span::terminal
