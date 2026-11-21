#include "core/external_tool_locations.h"

#include <QDir>
#include <QRegularExpression>
#include <QSettings>

namespace span::core {

namespace {

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

} // namespace

QString executableFileName(const QString &baseName)
{
    return baseName + QStringLiteral(".exe");
}

QStringList persistedSearchPath()
{
    // Windows composes a new process PATH as machine entries, then user ones.
    const QSettings machine(
        QStringLiteral("HKEY_LOCAL_MACHINE\\SYSTEM\\CurrentControlSet\\Control\\Session Manager\\Environment"),
        QSettings::NativeFormat);
    const QSettings user(QStringLiteral("HKEY_CURRENT_USER\\Environment"), QSettings::NativeFormat);
    return expandEnvironment(machine.value(QStringLiteral("Path")).toString())
               .split(QDir::listSeparator(), Qt::SkipEmptyParts)
        + expandEnvironment(user.value(QStringLiteral("Path")).toString())
              .split(QDir::listSeparator(), Qt::SkipEmptyParts);
}

QStringList knownToolDirectories(ExternalTool tool)
{
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
}

} // namespace span::core
