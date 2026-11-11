#pragma once

#include <QDir>
#include <QString>

namespace span::core {

// Sole owner of durable product files: everything the product writes lives
// under root(). The root is, first match wins:
//   1. the --data-dir <path> override (dev runs, smoke, packaging probes);
//   2. <exe dir>\data when portable.txt sits next to the executable;
//   3. %LOCALAPPDATA%\SpanTui, from the LOCALAPPDATA environment variable.
// QStandardPaths and Qt test mode are not consulted.
class ProductPaths final {
public:
    ProductPaths() = delete;

    // Marker file next to the executable that turns on portable mode.
    static QString portableMarkerName() { return QStringLiteral("portable.txt"); }
    // Data directory a portable image keeps next to its executable.
    static QString portableDataDirName() { return QStringLiteral("data"); }

    // Pure resolution used by root(); exposed for tests.
    static QString resolveRoot(const QString &overridePath, const QString &applicationDir, const QString &localAppData);

    // Moves %APPDATA%\Span\SpanTui to a default root() that does not exist
    // yet. Call before anything writes there; returns a warning or empty.
    static QString moveLegacyDefaultRoot();
    // Pure move used by moveLegacyDefaultRoot(); exposed for tests. Also
    // removes the legacy folder's parent when it is left empty.
    static QString moveLegacyRoot(const QString &legacyRoot, const QString &root);

    // --data-dir <path>: makes <path> the root. Empty clears it. Child
    // SpanTui processes are started with the same override.
    static void setRootOverride(const QString &path);
    static QString rootOverride();

    // Moves settings written by older builds (<root>\Span\SpanTui.ini) to
    // settingsPath() and binds default QSettings (IniFormat) into root(), so
    // nothing falls back to the registry.
    static void installApplicationSettings();

    static QString root();
    // True when the root comes from the portable marker.
    static bool isPortable();

    static QString databasePath()
    {
        return QDir(root()).filePath(QStringLiteral("span_tui.db"));
    }

    static QString settingsPath()
    {
        return QDir(root()).filePath(QStringLiteral("settings.ini"));
    }

    static QString launchLogPath()
    {
        return QDir(root()).filePath(QStringLiteral("last-launch.log"));
    }

    static QString crashLogDirectory() { return root(); }
    // Logs of helper processes (the update installer) live under root()/logs.
    static QString logDirectory()
    {
        return QDir(root()).filePath(QStringLiteral("logs"));
    }
    // Rebuildable caches (Qt graphics pipeline cache) live under root()/cache.
    static QString cacheDirectory()
    {
        return QDir(root()).filePath(QStringLiteral("cache"));
    }
    // Qt Quick pipeline cache of this image's workspace window.
    static QString pipelineCachePath();
    // Scratch data of --isolated-workspace runs, one locked subdirectory per run.
    static QString isolatedRunsDirectory()
    {
        return QDir(root()).filePath(QStringLiteral("isolated"));
    }
    // Durable hang logs live under root()/diagnostics/hang.
    static QString hangDiagnosticDirectory();
    // Present while this image brings up its graphics, from just before the
    // first window until its first frame or a clean exit. Per image name so
    // SpanTui and SpanTui-pre sharing a root do not read each other's.
    static QString graphicsStartMarkerPath();
};

} // namespace span::core
