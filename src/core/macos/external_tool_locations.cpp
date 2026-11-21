#include "core/external_tool_locations.h"

#include <QDir>
#include <QFile>

namespace span::core {

namespace {

constexpr QLatin1StringView kPathsFile{"/etc/paths"};
constexpr QLatin1StringView kPathsDirectory{"/etc/paths.d"};
// Homebrew on Apple Silicon, then the Intel-era prefix.
constexpr QLatin1StringView kHomebrewBin{"/opt/homebrew/bin"};
constexpr QLatin1StringView kLocalBin{"/usr/local/bin"};
constexpr QLatin1StringView kSystemBin{"/usr/bin"};
constexpr QLatin1StringView kTailscaleApp{"/Applications/Tailscale.app/Contents/MacOS"};

QStringList linesOf(const QString &path)
{
    QFile file(path);
    QStringList lines;
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) return lines;
    while (!file.atEnd()) {
        const QString line = QString::fromUtf8(file.readLine()).trimmed();
        if (!line.isEmpty() && !line.startsWith(QLatin1Char('#'))) lines << line;
    }
    return lines;
}

} // namespace

QString executableFileName(const QString &baseName)
{
    return baseName;
}

QStringList persistedSearchPath()
{
    QStringList entries = linesOf(QString(kPathsFile));
    const QDir directory{QString(kPathsDirectory)};
    for (const QString &name : directory.entryList(QDir::Files, QDir::Name)) {
        entries << linesOf(directory.filePath(name));
    }
    return entries;
}

QStringList knownToolDirectories(ExternalTool tool)
{
    switch (tool) {
    case ExternalTool::Git: return {QString(kHomebrewBin), QString(kLocalBin), QString(kSystemBin)};
    case ExternalTool::Ssh:
    case ExternalTool::Sftp:
        // The system OpenSSH client.
        return {QString(kSystemBin)};
    case ExternalTool::Tailscale: return {QString(kTailscaleApp), QString(kHomebrewBin), QString(kLocalBin)};
    }
    return {};
}

} // namespace span::core
