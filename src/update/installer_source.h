#pragma once

#include <QCoreApplication>
#include <QString>
#include <QVariantMap>

// What the running image is, and the local installer that can update it: the
// Setup the newest `dev package` output lists in its release-manifest.json.
// Pure file-system work: safe on a worker thread.
class InstallerSource final {
    Q_DECLARE_TR_FUNCTIONS(InstallerSource)

public:
    enum class ImageKind { Installed, Portable, Development };

    // Portable: portable.txt beside the executable. Installed: Inno Setup's
    // uninstaller (unins*.exe) beside it. Anything else, such as a build
    // tree, is a development image. Only an installed image runs a Setup.
    static ImageKind imageKind(const QString &imageDirectory);

    struct Result {
        bool available = false;
        // The folder is no usable source (as opposed to holding nothing newer).
        bool failed = false;
        QString reason;
        QString installer;
        // Lower-case hex SHA-256 the manifest lists for the installer.
        QString sha256;
        QString availableVersion;
    };

    // `folder` holds one folder per packaged version, laid out as `dev package`
    // writes them: <version>\windows-x64\release-manifest.json beside the
    // SpanTui-<version>-Setup.exe it lists. The newest package (version, then
    // builtAtUtc) is the candidate; `imageDirectory` holds the running image's
    // build stamp it is compared with.
    static Result probe(const QString &folder, const QString &imageDirectory, const QString &currentVersion);

    // SpanTui-<version>-Setup.exe, the name `dev package` gives the installer
    // and a GitHub release carries it under.
    static bool isInstallerName(const QString &fileName);
    // Version first; equal versions compare builtAtUtc when both are known.
    static bool isNewer(const QString &version, const QString &builtAtUtc, const QString &currentVersion,
                        const QString &currentBuiltAtUtc);
    static QVariantMap readBuildStamp(const QString &directory);
    static QString stampFileName();
    static QString manifestFileName();
    // The per-platform folder under each version folder.
    static QString platformSlot();
};
