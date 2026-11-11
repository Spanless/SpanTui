#pragma once

#include "application/graphics_mode_service.h"
#include "application/idle_governor.h"
#include "application/preferences.h"
#include "application/project_workspace_registry.h"
#include "application/shell_profile_registry.h"
#include "application/shortcut_registry.h"
#include "application/task_progress.h"
#include "application/task_progress_binding.h"
#include "application/tool_registry.h"
#include "application/ui_icon_catalog.h"
#include "application/window_transition_coordinator.h"
#include "application/workspace_controller.h"
#include "monitoring/resource_monitor.h"
#include "platform/diagnostics/graphics_startup.h"
#include "platform/windows/window_chrome.h"
#include "remote/remote_host_registry.h"
#include "remote/ssh_askpass.h"
#include "remote/tailscale_devices.h"
#include "terminal/api/terminal_manager.h"
#include "update/update_service.h"
#include "workspace/file_icon_theme.h"
#include "workspace/session_model.h"
#include "workspace/workspace_layout.h"

#include <memory>

class QGuiApplication;
class QQmlApplicationEngine;
class QQuickWindow;
class Translator;

namespace span::application {
struct LaunchOptions;
namespace diagnostics {
class UiHealthMonitor;
}
} // namespace span::application

namespace span::application::runtime {

struct InternalLaunchOptions;
class IsolatedRunData;

// Composition root of the full workspace process. Members are declared in
// dependency order, so a member only references members declared before it;
// the destructor tears QML down first, while every context object it can
// still reach during destruction is alive.
class WorkspaceRuntime final {
public:
    WorkspaceRuntime(QGuiApplication &app,
                     const LaunchOptions &launchOptions,
                     const InternalLaunchOptions &internal);
    ~WorkspaceRuntime();
    WorkspaceRuntime(const WorkspaceRuntime &) = delete;
    WorkspaceRuntime &operator=(const WorkspaceRuntime &) = delete;

    // Builds persistence, services, wiring and the QML window. Returns 0, or
    // the process exit code of the step that failed.
    int initialize();
    // Applies the visibility policy (headless, harness, restored geometry)
    // and starts the optional resource report. `raiseForActivation` reuses
    // the live activation path for requests that arrived before the window.
    void present(bool raiseForActivation);

    ProjectWorkspaceRegistry *projectRegistry() const { return m_projectRegistry.get(); }
    QQuickWindow *window() const { return m_window; }

private:
    int initializePersistence();
    void prepareGraphics();
    void wireErrors();
    void wireLayout();
    void wireContexts();
    void wireAppearance();
    void wireSessions();
    void wireLifecycle();
    bool loadQml();
    void attachWindow();
    void startResourceReport();
    void wireProjectContext(ProjectContext *context);
    void wireRemoteContext(span::remote::RemoteHostContext *context);

    QGuiApplication &m_app;
    const LaunchOptions &m_launchOptions;
    const InternalLaunchOptions &m_internal;

    SessionModel m_sessionModel;
    span::terminal::TerminalManager m_terminalManager;
    FileIconTheme m_fileIconTheme;
    UiIconCatalog m_uiIcons;
    Preferences m_preferences;
    ShortcutRegistry m_shortcuts;
    std::unique_ptr<IsolatedRunData> m_visualHarnessData;
    ResourceMonitor m_resourceMonitor;
    UpdateService m_updateService;
    TaskProgressService m_taskProgress;
    std::unique_ptr<ToolRegistry> m_tools;
    std::unique_ptr<ShellProfileRegistry> m_shells;
    std::unique_ptr<span::platform::diagnostics::GraphicsStartupWatch> m_graphicsWatch;
    std::unique_ptr<GraphicsModeService> m_graphics;
    std::unique_ptr<ProjectWorkspaceRegistry> m_projectRegistry;
    span::remote::SshAskpassBroker m_askpassBroker;
    std::unique_ptr<span::remote::RemoteHostRegistry> m_remoteHosts;
    span::remote::TailscaleDeviceModel m_tailscaleDevices;
    IdleGovernor m_idleGovernor;
    span::workspace::WorkspaceLayout m_workspaceLayout;
    std::unique_ptr<WorkspaceController> m_workspaceController;
    WindowChrome m_windowChrome;
    WindowTransitionCoordinator m_windowTransition;
    std::unique_ptr<QQmlApplicationEngine> m_engine;
    std::unique_ptr<Translator> m_translator;
    std::unique_ptr<diagnostics::UiHealthMonitor> m_uiHealthMonitor;
    QQuickWindow *m_window = nullptr;
};

} // namespace span::application::runtime
