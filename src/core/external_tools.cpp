#include "core/external_tools.h"

#include "core/external_tool_locations.h"

#include <QDir>
#include <QFileInfo>
#include <QSet>
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
        // Both platforms' default file systems ignore case.
        const QString key = clean.toCaseFolded();
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
    const QString name = executableFileName(toolId(tool));
    const QStringList known = knownToolDirectories(tool);
    // The system OpenSSH client wins over other ssh copies on PATH (on Windows
    // Git's MSYS build cannot use the askpass pipe); Git and
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
