#include "core/product_paths.h"

#include "core/core_constants.h"
#include "core/product_identity.h"

#include <QCoreApplication>
#include <QDebug>
#include <QFile>
#include <QFileInfo>
#include <QSettings>

namespace span::core {
namespace {

QString g_rootOverride;

QString applicationDirectory()
{
    return QCoreApplication::instance() ? QCoreApplication::applicationDirPath()
                                        : QString();
}

QString knownFolder(const char *variable, const QString &homeRelative)
{
    const QString folder = qEnvironmentVariable(variable);
    return folder.isEmpty() ? QDir(QDir::homePath()).filePath(homeRelative) : folder;
}

QString localAppDataDirectory()
{
    return knownFolder("LOCALAPPDATA", QStringLiteral("AppData/Local"));
}

QString legacyDefaultRoot()
{
    return QDir(knownFolder("APPDATA", QStringLiteral("AppData/Roaming"))).filePath(QStringLiteral("Span/SpanTui"));
}

bool hasPortableMarker(const QString &applicationDir)
{
    return !applicationDir.isEmpty()
        && QFileInfo(QDir(applicationDir).filePath(ProductPaths::portableMarkerName()))
               .isFile();
}

} // namespace

QString ProductPaths::resolveRoot(const QString &overridePath, const QString &applicationDir,
                                  const QString &localAppData)
{
    if (!overridePath.isEmpty()) {
        return QDir::cleanPath(QDir(overridePath).absolutePath());
    }
    return hasPortableMarker(applicationDir) ? QDir(QDir::cleanPath(applicationDir)).filePath(portableDataDirName())
                                             : QDir(QDir::cleanPath(localAppData)).filePath(kProductName);
}

QString ProductPaths::moveLegacyDefaultRoot()
{
    if (!g_rootOverride.isEmpty() || hasPortableMarker(applicationDirectory())) {
        return QString();
    }
    return moveLegacyRoot(legacyDefaultRoot(), root());
}

QString ProductPaths::moveLegacyRoot(const QString &legacyRoot, const QString &root)
{
    if (!QFileInfo(legacyRoot).isDir() || QFileInfo::exists(root)) {
        return QString();
    }
    // A failed move leaves the old folder untouched; this launch then starts
    // with an empty root, and the warning names both folders.
    if (!QDir().mkpath(QFileInfo(root).absolutePath()) || !QDir().rename(legacyRoot, root)) {
        return QStringLiteral("Could not move the data folder %1 to %2; it was left in place.")
            .arg(QDir::toNativeSeparators(legacyRoot), QDir::toNativeSeparators(root));
    }
    QDir().rmdir(QFileInfo(legacyRoot).absolutePath());
    return QString();
}

void ProductPaths::setRootOverride(const QString &path)
{
    g_rootOverride = path;
}

QString ProductPaths::rootOverride()
{
    return g_rootOverride;
}

QString ProductPaths::root()
{
    return resolveRoot(g_rootOverride, applicationDirectory(), localAppDataDirectory());
}

bool ProductPaths::isPortable()
{
    return g_rootOverride.isEmpty() && hasPortableMarker(applicationDirectory());
}

QString ProductPaths::hangDiagnosticDirectory()
{
    return QDir(root()).filePath(QStringLiteral("diagnostics/hang"));
}

QString ProductPaths::pipelineCachePath()
{
    return QDir(cacheDirectory()).filePath(productImageName() + QStringLiteral(".qtpipelinecache"));
}

QString ProductPaths::graphicsStartMarkerPath()
{
    return QDir(root()).filePath(productImageName() + QStringLiteral(".graphics-start"));
}

void ProductPaths::installApplicationSettings()
{
    const QString directory = root();
    QDir().mkpath(directory);
    // Builds before settings.ini kept default QSettings at <root>\Span\SpanTui.ini.
    const QString legacyDirectory = QDir(directory).filePath(QStringLiteral("Span"));
    const QString legacy = QDir(legacyDirectory).filePath(QStringLiteral("SpanTui.ini"));
    if (QFileInfo(legacy).isFile() && !QFileInfo::exists(settingsPath())) {
        if (QFile::rename(legacy, settingsPath())) {
            QDir(directory).rmdir(QStringLiteral("Span"));
        } else {
            qWarning().noquote() << "Could not move legacy settings" << legacy
                                 << "to" << settingsPath();
        }
    }
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, directory);
    QSettings::setPath(QSettings::IniFormat, QSettings::SystemScope, directory);
}

} // namespace span::core
