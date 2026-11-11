#include "application/runtime/terminal_application.h"

#include "application/application_constants.h"
#include "application/graphics_mode_service.h"
#include "application/launch_options.h"
#include "application/message_center.h"
#include "application/preferences.h"
#include "application/runtime/qml_types.h"
#include "application/shell_profile_registry.h"
#include "application/shortcut_registry.h"
#include "application/terminal_host_events.h"
#include "application/ui_icon_catalog.h"
#include "application/window_transition_coordinator.h"
#include "application/diagnostics/ui_health_monitor.h"
#include "core/product_paths.h"
#include "platform/diagnostics/graphics_startup.h"
#include "platform/windows/window_chrome.h"
#include "terminal/api/terminal_manager.h"
#include "terminal/core/terminal_color.h"
#include "terminal/terminal_constants.h"

#include <QCoreApplication>
#include <QGuiApplication>
#include <QQmlApplicationEngine>
#include <QQuickWindow>
#include <QStyleHints>
#include <QUuid>

namespace span::application::runtime {

int runTerminalApplication(QGuiApplication &app,
                           const LaunchOptions &options,
                           bool headless)
{
    // Declared before the engine so the singletons outlive every QML object.
    Preferences preferences;
    // One path: the logging handler files a critical as one Error message.
    const auto reportError = [](const QString &message) {
        qCritical().noquote() << "Terminal preferences error:" << message;
    };
    QObject::connect(&preferences.store, &SettingsStore::errorOccurred, &app, reportError);
    QObject::connect(&preferences.terminal, &TerminalSettings::errorOccurred, &app, reportError);
    if (!preferences.open(SettingsStore::Mode::ReadOnly)) {
        qCritical("Failed to initialize terminal preferences.");
        return 2;
    }
    AppearanceSettings &appearance = preferences.appearance;
    appearance.setSystemDark(app.styleHints()->colorScheme() == Qt::ColorScheme::Dark);
    appearance.setSystemWheelScrollLines(app.styleHints()->wheelScrollLines());
    appearance.refreshSystemReduceMotion();
    QGuiApplication::setFont(appearance.uiFont(appearance.uiFontSize()));
    // Same rendering choice as the workspace. Preferences are read-only
    // here, so this mode keeps no start marker of its own.
    const GraphicsModeService graphics(&preferences.workspace, options.safeGraphics, false);
    if (graphics.softwareActive()) {
        span::platform::diagnostics::preferSoftwareRendering();
    }
    QQuickWindow::setTextRenderType(QQuickWindow::QtTextRendering);

    ShortcutRegistry shortcuts(&preferences.workspace);
    UiIconCatalog uiIcons;
    WindowTransitionCoordinator windowTransition;
    WindowChrome windowFacts;
    span::terminal::TerminalManager terminalManager;
    QObject::connect(&terminalManager, &span::terminal::TerminalManager::errorOccurred, &app, reportError);
    // Standalone mode has no task-progress surface; notifications still land.
    connectTerminalHostEvents(&terminalManager, nullptr);
    const auto applyTerminalSurfaceColors = [&terminalManager, &preferences]() {
        const span::terminal::TerminalColorScheme scheme = preferences.terminal.activeColorScheme();
        terminalManager.setSurfaceColors(
            scheme.foreground, scheme.background, scheme.ansi);
    };
    applyTerminalSurfaceColors();
    QObject::connect(&preferences.terminal, &TerminalSettings::appearanceChanged, &app, applyTerminalSurfaceColors);
    const QString sessionId =
        QUuid::createUuid().toString(QUuid::WithoutBraces);
    QVariantMap terminalOptions;
    // Without an explicit command the window starts the default shell profile.
    const QString commandLine = !options.commandLine.isEmpty()
        ? options.commandLine
        : ShellProfileRegistry(&preferences.terminal).commandLine({});
    if (!commandLine.isEmpty()) {
        terminalOptions.insert(terminal::kSessionCommandLineKey, commandLine);
    }
    if (!terminalManager.createSession(sessionId, options.terminalCwd,
                                       terminalOptions)) {
        qCritical("Failed to create transient terminal session.");
        return 3;
    }
    terminalManager.setActiveSession(sessionId);

    // The services this window's QML reaches (qml_types.h).
    exposePreferences(preferences);
    QmlInstance<ShortcutRegistry>::set(&shortcuts);
    QmlInstance<MessageCenter>::set(MessageCenter::instance());
    QmlInstance<span::terminal::TerminalManager>::set(&terminalManager);
    QmlInstance<UiIconCatalog>::set(&uiIcons);
    QmlInstance<WindowTransitionCoordinator>::set(&windowTransition);
    QQmlApplicationEngine engine;
    QObject::connect(&engine,
                     &QQmlApplicationEngine::objectCreationFailed,
                     &app, []() { QCoreApplication::exit(-1); },
                     Qt::QueuedConnection);
    engine.loadFromModule(kQmlModuleUri, QStringLiteral("TerminalPanel"));
    auto *window =
        qobject_cast<QQuickWindow *>(engine.rootObjects().value(0));
    if (!window) {
        return -1;
    }
    // Any number of terminal windows run at once: read the workspace's cache,
    // never write it. The window is exposed on the next event loop turn.
    span::platform::diagnostics::configurePipelineCache(
        window, span::core::ProductPaths::pipelineCachePath(), false);
    QObject::connect(&windowFacts, &WindowChrome::interactiveResizeStarted,
                     &windowTransition,
                     &WindowTransitionCoordinator::beginNativeResize);
    QObject::connect(&windowFacts, &WindowChrome::interactiveResizeEnded,
                     &windowTransition,
                     &WindowTransitionCoordinator::endNativeResize);
    QObject::connect(&windowFacts, &WindowChrome::nativeWindowStateChanged,
                     &windowTransition,
                     &WindowTransitionCoordinator::noteNativeStateChange);
    QObject::connect(&windowFacts, &WindowChrome::devicePixelRatioChanged,
                     &windowTransition,
                     &WindowTransitionCoordinator::noteDpiChange);
    span::platform::diagnostics::GraphicsStartupWatch graphicsWatch{QString()};
    graphicsWatch.watch(window, graphics.softwareActive());
    windowFacts.observeTransitions(window);
    windowTransition.attachWindow(window);
    span::application::diagnostics::UiHealthMonitor uiHealthMonitor(
        &windowTransition, &terminalManager, nullptr, nullptr, window);
    uiHealthMonitor.start();
    if (headless) {
        window->setVisibility(QWindow::Hidden);
        qInfo().noquote()
            << "SpanTui terminal headless mode: root window created, not shown.";
    }
    return app.exec();
}

} // namespace span::application::runtime
