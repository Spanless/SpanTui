#include "application/runtime/internal_launch_options.h"

namespace span::application::runtime {
namespace {

QSize parseSize(const QString &value)
{
    const QStringList parts = value.split(QLatin1Char('x'));
    if (parts.size() != 2) {
        return {};
    }
    bool widthValid = false;
    bool heightValid = false;
    const int width = parts.at(0).toInt(&widthValid);
    const int height = parts.at(1).toInt(&heightValid);
    return widthValid && heightValid && width > 0 && height > 0
               ? QSize(width, height)
               : QSize();
}

} // namespace

InternalLaunchOptions parseInternalLaunchOptions(const QStringList &arguments)
{
    InternalLaunchOptions options;
    for (const QString &argument : arguments) {
        // Automation flags stay available in Release images: packaging and
        // worker smoke runs must never show UI or touch the live product
        // database. The visual harness and --resource-report are Debug-only.
        if (argument == QLatin1String("--headless")
            || argument == QLatin1String("--no-window")) {
            options.headless = true;
        } else if (argument == QLatin1String("--isolated-workspace")) {
            options.isolatedWorkspace = true;
        } else if (argument == QLatin1String("--conpty=system")) {
            options.systemConPty = true;
        } else if (argument == QLatin1String("--trace-viewport")
                   || argument == QLatin1String("--trace-viewport=1")) {
            options.traceViewport = true;
        } else if (argument == QLatin1String("--trace-viewport=0")) {
            options.traceViewport = false;
        }
#ifndef NDEBUG
        const auto value = [&argument](QLatin1String prefix, QString *out) {
            if (!argument.startsWith(prefix)) {
                return false;
            }
            *out = argument.mid(prefix.size());
            return true;
        };
        QString text;
        if (value(QLatin1String("--boundary-gallery-shot="), &options.boundaryGalleryShotPath)
            || value(QLatin1String("--boundary-gallery-theme="),
                     &options.boundaryGalleryTheme)) {
            options.boundaryGallery = true;
        } else if (value(QLatin1String("--workspace-shot="), &options.workspaceShotPath)
                   || value(QLatin1String("--workspace-shot-theme="),
                            &options.workspaceShotTheme)
                   || value(QLatin1String("--workspace-shot-page="),
                            &options.workspaceShotPage)
                   || value(QLatin1String("--workspace-shot-right-tool="),
                            &options.workspaceShotRightTool)
                   || value(QLatin1String("--workspace-shot-project="),
                            &options.workspaceShotProjectPath)
                   || value(QLatin1String("--workspace-shot-file="),
                            &options.workspaceShotFilePath)) {
            options.workspaceShot = true;
        } else if (value(QLatin1String("--workspace-shot-delay-ms="), &text)) {
            options.workspaceShot = true;
            options.workspaceShotDelayMs = qBound(500, text.toInt(), 30000);
        } else if (value(QLatin1String("--workspace-shot-viewport="), &text)) {
            const QSize size = parseSize(text);
            if (size.isValid()) {
                options.workspaceShot = true;
                options.workspaceShotViewport = size;
            }
        } else if (argument == QLatin1String("--workspace-shot-terminal")) {
            options.workspaceShot = true;
            options.workspaceShotTerminal = true;
        } else {
            value(QLatin1String("--resource-report="), &options.resourceReportPath);
        }
#endif
    }
    return options;
}

} // namespace span::application::runtime
