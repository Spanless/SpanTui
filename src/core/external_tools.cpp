#include "core/external_tools.h"

#include <QDir>
#include <QFileInfo>
#include <QRegularExpression>
#include <QSet>
#include <QSettings>
#include <QStandardPaths>

namespace span::core {
namespace {

constexpr int kToolCount = 4;

struct ToolSlot {
    bool published = false;
    QString path;
    // A found default is kept; a missing one is looked up again next time.
    QString defaultPath;
};

ToolSlot g_tools[kToolCount];

ToolSlot &slot(ExternalTool tool)
{
    return g_tools[int(tool)];
}

QString executableName(const QString &base)
{
#ifdef Q_OS_WIN
    return base + QStringLiteral(".exe");
#else
    return base;
#endif
}

// %NAME% references as stored in REG_EXPAND_SZ values; unknown names stay.
QString expandEnvironment(const QString &value)
{
    static const QRegularExpression variable(QStringLiteral("%([^%]+)%"));
    QString result;
    qsizetype last = 0;
    auto matches = variable.globalMatch(value);
    while (matches.hasNext()) {
        const auto match = matches.next();
        result += value.mid(last, match.capturedStart() - last);
        const QString name = match.captured(1);
        result += qEnvironmentVariableIsSet(name.toLocal8Bit().constData())
            ? qEnvironmentVariable(name.toLocal8Bit().constData())
            : match.captured(0);
        last = match.capturedEnd();
    }
    return result + value.mid(last);
}

QStringList persistedSearchPath()
{
#ifdef Q_OS_WIN
    // Windows composes a new process PATH as machine entries, then user ones.
    const QSettings machine(
        QStringLiteral("HKEY_LOCAL_MACHINE\\SYSTEM\\CurrentControlSet\\Control\\Session Manager\\Environment"),
        QSettings::NativeFormat);
    const QSettings user(QStringLiteral("HKEY_CURRENT_USER\\Environment"),
                         QSettings::NativeFormat);
    return expandEnvironment(machine.value(QStringLiteral("Path")).toString())
               .split(QDir::listSeparator(), Qt::SkipEmptyParts)
        + expandEnvironment(user.value(QStringLiteral("Path")).toString())
              .split(QDir::listSeparator(), Qt::SkipEmptyParts);
#else
    return {};
#endif
}

QStringList knownToolDirectories(ExternalTool tool)
{
#ifdef Q_OS_WIN
    const auto under = [](const char *variable, const QString &relative) {
        const QString root = qEnvironmentVariable(variable);
        return root.isEmpty() ? QString() : QDir(root).filePath(relative);
    };
    QStringList directories;
    switch (tool) {
    case ExternalTool::Git:
        // Git for Windows: machine-wide and per-user installer locations.
        directories << under("ProgramW6432", QStringLiteral("Git/cmd"))
                    << under("ProgramFiles", QStringLiteral("Git/cmd"))
                    << under("ProgramFiles(x86)", QStringLiteral("Git/cmd"))
                    << under("LOCALAPPDATA", QStringLiteral("Programs/Git/cmd"));
        break;
    case ExternalTool::Ssh:
    case ExternalTool::Sftp:
        // The Windows optional feature "OpenSSH Client".
        directories << under("SystemRoot", QStringLiteral("System32/OpenSSH"));
        break;
    case ExternalTool::Tailscale:
        // The Tailscale client installer puts its CLI next to the service.
        directories << under("ProgramW6432", QStringLiteral("Tailscale"))
                    << under("ProgramFiles", QStringLiteral("Tailscale"))
                    << under("ProgramFiles(x86)", QStringLiteral("Tailscale"));
        break;
    }
    directories.removeAll(QString());
    return directories;
#else
    Q_UNUSED(tool);
    return {};
#endif
}

} // namespace

QString toolId(ExternalTool tool)
{
    switch (tool) {
    case ExternalTool::Git:
        return QStringLiteral("git");
    case ExternalTool::Ssh:
        return QStringLiteral("ssh");
    case ExternalTool::Sftp:
        return QStringLiteral("sftp");
    case ExternalTool::Tailscale: return QStringLiteral("tailscale");
    }
    return {};
}

QStringList executableSearchPath()
{
    QStringList directories;
    QSet<QString> seen;
    const QStringList entries =
        qEnvironmentVariable("PATH").split(QDir::listSeparator(), Qt::SkipEmptyParts)
        + persistedSearchPath();
    for (QString entry : entries) {
        entry = entry.trimmed();
        if (entry.size() >= 2 && entry.startsWith(QLatin1Char('"'))
            && entry.endsWith(QLatin1Char('"'))) {
            entry = entry.mid(1, entry.size() - 2);
        }
        if (entry.isEmpty() || !QDir::isAbsolutePath(entry)) {
            continue;
        }
        const QString clean = QDir::cleanPath(entry);
#ifdef Q_OS_WIN
        const QString key = clean.toCaseFolded();
#else
        const QString key = clean;
#endif
        if (!seen.contains(key)) {
            seen.insert(key);
            directories << clean;
        }
    }
    return directories;
}

QString findExecutableIn(const QString &fileName, const QStringList &directories)
{
    // QStandardPaths falls back to the process PATH for an empty list.
    if (directories.isEmpty()) {
        return {};
    }
    const QString found = QStandardPaths::findExecutable(fileName, directories);
    return found.isEmpty() ? QString() : QDir::toNativeSeparators(found);
}

QString defaultToolExecutable(ExternalTool tool)
{
    const QString name = executableName(toolId(tool));
    const QStringList known = knownToolDirectories(tool);
    // The system OpenSSH client wins over other ssh.exe copies on PATH (Git's
    // MSYS build, for one, cannot use the Windows askpass pipe); Git and
    // Tailscale prefer the user's PATH and fall back to their install folders.
    if (tool == ExternalTool::Ssh || tool == ExternalTool::Sftp) {
        const QString system = findExecutableIn(name, known);
        return system.isEmpty() ? findExecutableIn(name, executableSearchPath()) : system;
    }
    const QString onPath = findExecutableIn(name, executableSearchPath());
    return onPath.isEmpty() ? findExecutableIn(name, known) : onPath;
}

QString toolExecutable(ExternalTool tool)
{
    ToolSlot &entry = slot(tool);
    if (entry.published) {
        return entry.path;
    }
    if (entry.defaultPath.isEmpty() || !QFileInfo(entry.defaultPath).isFile()) {
        entry.defaultPath = defaultToolExecutable(tool);
    }
    return entry.defaultPath;
}

void setToolExecutable(ExternalTool tool, const QString &path)
{
    ToolSlot &entry = slot(tool);
    entry.published = true;
    entry.path = path;
}

} // namespace span::core
