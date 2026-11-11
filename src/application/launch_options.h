#pragma once

#include <QString>
#include <QStringList>

namespace span::application {

struct LaunchOptions {
    bool terminalMode = false;
    // Software rendering (WARP) for this launch only; see GraphicsModeService.
    bool safeGraphics = false;
    QString projectPath;
    // --data-dir <absolute-dir>: data root for this launch instead of the
    // portable or %LOCALAPPDATA% root; created when missing.
    QString dataDir;
    QString terminalCwd;
    QString commandLine;
    QString error;

    bool valid() const { return error.isEmpty(); }
};

LaunchOptions parseLaunchOptions(const QStringList &arguments);

} // namespace span::application
