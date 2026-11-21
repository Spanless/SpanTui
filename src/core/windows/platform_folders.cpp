#include "core/platform_folders.h"

#include <QDir>

namespace span::core {

namespace {

QString knownFolder(const char *variable, const QString &homeRelative)
{
    const QString folder = qEnvironmentVariable(variable);
    return folder.isEmpty() ? QDir(QDir::homePath()).filePath(homeRelative) : folder;
}

} // namespace

QString userDataParentDirectory()
{
    return knownFolder("LOCALAPPDATA", QStringLiteral("AppData/Local"));
}

QString legacyDefaultDataRoot()
{
    return QDir(knownFolder("APPDATA", QStringLiteral("AppData/Roaming"))).filePath(QStringLiteral("Span/SpanTui"));
}

} // namespace span::core
