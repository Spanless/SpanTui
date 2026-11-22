#include "platform/local_endpoint.h"

#include <QDir>
#include <QFile>

#include <unistd.h>

namespace span::platform {

namespace {

QString userTemporaryDirectory()
{
    const size_t size = confstr(_CS_DARWIN_USER_TEMP_DIR, nullptr, 0);
    if (size == 0) return QDir::tempPath();
    QByteArray path(qsizetype(size), '\0');
    if (confstr(_CS_DARWIN_USER_TEMP_DIR, path.data(), size) == 0) return QDir::tempPath();
    path.truncate(path.indexOf('\0'));
    return QDir::cleanPath(QFile::decodeName(path));
}

} // namespace

QString localEndpointAddress(const QString &name)
{
    static const QString directory = userTemporaryDirectory();
    return QDir(directory).filePath(name);
}

} // namespace span::platform
