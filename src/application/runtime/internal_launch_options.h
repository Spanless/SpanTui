#pragma once

#include "application/application_constants.h"

#include <QSize>
#include <QString>
#include <QStringList>

#include <optional>

namespace span::application::runtime {

struct InternalLaunchOptions {
    QString resourceReportPath;
    bool headless = false;
    bool isolatedWorkspace = false;
    // Diagnostics: --conpty=system compares against the system console host;
    // --trace-viewport[=0|1] overrides the build-type default of
    // TerminalViewportTrace.
    bool systemConPty = false;
    std::optional<bool> traceViewport;

    bool boundaryGallery = false;
    QString boundaryGalleryShotPath;
    QString boundaryGalleryTheme = kThemeDark;

    bool workspaceShot = false;
    QString workspaceShotPath;
    QString workspaceShotTheme = kThemeDark;
    QString workspaceShotPage = QStringLiteral("workspace");
    QString workspaceShotRightTool;
    int workspaceShotDelayMs = 1600;
    bool workspaceShotTerminal = false;
    QString workspaceShotProjectPath;
    QString workspaceShotFilePath;
    QSize workspaceShotViewport = QSize(1600, 900);

    // Visual harness runs own their data root and never join (or forward to)
    // the single workspace instance.
    bool isolatedVisualHarness() const {
        return boundaryGallery || workspaceShot || isolatedWorkspace;
    }
};

InternalLaunchOptions parseInternalLaunchOptions(const QStringList &arguments);

} // namespace span::application::runtime
