#include "update/installer_source.h"

#include "core/product_paths.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QVersionNumber>

namespace {

constexpr auto kStampFile = "spantui-build.json";
constexpr auto kManifestFile = "release-manifest.json";
constexpr auto kPlatformSlot = "windows-x64";

struct Package {
    QString directory;
    QString version;
    QString builtAtUtc;
    QJsonObject manifest;
};

// Empty version when the manifest is missing or unreadable.
Package readPackage(const QString &directory)
{
    Package package;
    package.directory = directory;
    QFile file(QDir(directory).filePath(QString::fromLatin1(kManifestFile)));
    if (!file.open(QIODevice::ReadOnly)) {
        return package;
    }
    const QJsonDocument doc = QJsonDocument::fromJson(file.readAll());
    package.manifest = doc.object();
    package.version = package.manifest.value(QStringLiteral("version")).toString().trimmed();
    package.builtAtUtc = package.manifest.value(QStringLiteral("builtAtUtc")).toString();
    return package;
}

} // namespace

QString InstallerSource::stampFileName()
{
    return QString::fromLatin1(kStampFile);
}
QString InstallerSource::manifestFileName()
{
    return QString::fromLatin1(kManifestFile);
}
QString InstallerSource::platformSlot()
{
    return QString::fromLatin1(kPlatformSlot);
}

InstallerSource::ImageKind InstallerSource::imageKind(const QString &imageDirectory)
{
    const QDir directory(imageDirectory);
    if (QFileInfo::exists(directory.filePath(span::core::ProductPaths::portableMarkerName()))) {
        return ImageKind::Portable;
    }
    if (!directory.entryList({QStringLiteral("unins*.exe")}, QDir::Files).isEmpty()) {
        return ImageKind::Installed;
    }
    return ImageKind::Development;
}

bool InstallerSource::isInstallerName(const QString &fileName)
{
    static const QRegularExpression installer(QStringLiteral("^SpanTui-[^/\\\\]+-Setup\\.exe$"),
                                              QRegularExpression::CaseInsensitiveOption);
    return installer.match(fileName).hasMatch();
}

QVariantMap InstallerSource::readBuildStamp(const QString &directory)
{
    QFile file(QDir(directory).filePath(stampFileName()));
    if (!file.open(QIODevice::ReadOnly)) {
        return {};
    }
    const QJsonDocument doc = QJsonDocument::fromJson(file.readAll());
    return doc.isObject() ? doc.object().toVariantMap() : QVariantMap{};
}

bool InstallerSource::isNewer(const QString &version, const QString &builtAtUtc, const QString &currentVersion,
                              const QString &currentBuiltAtUtc)
{
    const QVersionNumber candidate = QVersionNumber::fromString(version);
    const QVersionNumber current = QVersionNumber::fromString(currentVersion);
    if (candidate.isNull()) {
        return false;
    }
    if (current.isNull() || candidate != current) {
        return current.isNull() || candidate > current;
    }
    // Same version: a later local package build. ISO 8601 UTC sorts as text.
    return !builtAtUtc.isEmpty() && !currentBuiltAtUtc.isEmpty() && builtAtUtc > currentBuiltAtUtc;
}

InstallerSource::Result InstallerSource::probe(const QString &folder, const QString &imageDirectory,
                                               const QString &currentVersion)
{
    Result result;
    result.failed = true;
    if (folder.trimmed().isEmpty()) {
        result.reason = tr("No update source path configured.");
        return result;
    }
    // A new version gets a new folder, so the source stays the same folder
    // from one release to the next.
    const QDir root(folder);
    Package newest;
    QString unreadable;
    for (const QString &name : root.entryList(QDir::Dirs | QDir::NoDotAndDotDot)) {
        const QString slot = QDir(root.filePath(name)).filePath(platformSlot());
        if (!QFileInfo::exists(QDir(slot).filePath(manifestFileName()))) {
            continue;
        }
        const Package package = readPackage(slot);
        if (package.version.isEmpty()) {
            unreadable = slot;
            continue;
        }
        if (newest.version.isEmpty()
            || isNewer(package.version, package.builtAtUtc, newest.version, newest.builtAtUtc)) {
            newest = package;
        }
    }
    if (newest.version.isEmpty()) {
        result.reason = unreadable.isEmpty()
            ? tr("The update source holds no packages; choose the folder above the version folders "
                 "dev package writes.")
            : tr("The %1 in %2 is not valid.").arg(manifestFileName(), QDir::toNativeSeparators(unreadable));
        return result;
    }

    // The newest package is the one on offer: an older one never stands in
    // for it, so a broken newest package is reported rather than skipped.
    result.availableVersion = newest.version;
    for (const QJsonValue &value : newest.manifest.value(QStringLiteral("artifacts")).toArray()) {
        const QJsonObject artifact = value.toObject();
        const QString file = artifact.value(QStringLiteral("file")).toString();
        if (isInstallerName(file)) {
            result.installer = QFileInfo(QDir(newest.directory).filePath(file)).absoluteFilePath();
            result.sha256 = artifact.value(QStringLiteral("sha256")).toString().trimmed().toLower();
            break;
        }
    }
    if (result.installer.isEmpty() || result.sha256.isEmpty()) {
        result.reason = tr("Package %1 lists no installer; dev package --quick builds none.").arg(newest.version);
        return result;
    }
    if (!QFileInfo(result.installer).isFile()) {
        result.reason = tr("The installer %1 is missing from package %2.")
                            .arg(QFileInfo(result.installer).fileName(), newest.version);
        return result;
    }

    result.failed = false;
    const QVariantMap stamp = readBuildStamp(imageDirectory);
    const QString stampVersion = stamp.value(QStringLiteral("version")).toString();
    if (!isNewer(newest.version, newest.builtAtUtc, stampVersion.isEmpty() ? currentVersion : stampVersion,
                 stamp.value(QStringLiteral("builtAtUtc")).toString())) {
        result.reason = tr("Already up to date with the local source.");
        return result;
    }
    result.available = true;
    result.reason = tr("Newer local package found.");
    return result;
}
