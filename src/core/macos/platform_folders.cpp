#include "core/platform_folders.h"

#include <QDir>

namespace span::core {

QString userDataParentDirectory()
{
    return QDir(QDir::homePath()).filePath(QStringLiteral("Library/Application Support"));
}

QString legacyDefaultDataRoot()
{
    return QString();
}

} // namespace span::core
