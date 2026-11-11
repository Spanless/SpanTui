#pragma once

// Declarative QML registration of every C++ type and process service in the
// SpanTui module. The build's type registrar reads these declarations, so the
// QML compiler knows each name and its properties and compiles the bindings
// that use them to C++; the domain libraries stay free of QML macros.
//
// Services are singletons over objects a host owns. A host hands its objects
// to QmlInstance<T>::set() before it creates the QML engine and keeps them
// alive past it; QML only reaches the services of its own host (the
// standalone terminal and the boundary gallery expose a subset).

#include "application/graphics_mode_service.h"
#include "application/icon_motion.h"
#include "application/message_center.h"
#include "application/pixel_snap.h"
#include "application/preferences.h"
#include "application/project_context.h"
#include "application/project_workspace_registry.h"
#include "application/shell_profile_registry.h"
#include "application/shortcut_registry.h"
#include "application/task_progress.h"
#include "application/tool_registry.h"
#include "application/translator.h"
#include "application/ui_icon_catalog.h"
#include "application/window_transition_coordinator.h"
#include "application/workspace_controller.h"
#include "documents/document_manager.h"
#include "documents/editor_controller.h"
#include "documents/markdown_document_renderer.h"
#include "git/git_repository_service.h"
#include "monitoring/resource_monitor.h"
#include "platform/windows/window_chrome.h"
#include "remote/remote_file_model.h"
#include "remote/remote_host_context.h"
#include "remote/remote_host_registry.h"
#include "remote/remote_system_monitor.h"
#include "remote/tailscale_devices.h"
#include "workspace/file_tree_model.h"
#include "terminal/api/terminal_manager.h"
#include "terminal/api/terminal_session.h"
#include "terminal/render/terminal_view.h"
#include "update/update_service.h"
#include "workspace/file_icon_theme.h"
#include "workspace/session_model.h"
#include "workspace/workspace_layout.h"

#include <QJSEngine>
#include <QPointer>
#include <QtQml/qqmlregistration.h>

class QQmlEngine;

namespace span::application::runtime {

// The object behind one service singleton. The engine asks for it on first
// use and keeps it for its lifetime; the host keeps ownership.
template <typename T> class QmlInstance final {
public:
    static void set(T *instance) { s_instance = instance; }
    static T *create()
    {
        T *instance = s_instance.data();
        if (instance) QJSEngine::setObjectOwnership(instance, QJSEngine::CppOwnership);
        return instance;
    }

private:
    inline static QPointer<T> s_instance;
};

// Appearance, TerminalSettings, WorkspaceSettings and Clipboard.
inline void exposePreferences(Preferences &preferences)
{
    QmlInstance<AppearanceSettings>::set(&preferences.appearance);
    QmlInstance<TerminalSettings>::set(&preferences.terminal);
    QmlInstance<WorkspaceSettings>::set(&preferences.workspace);
    QmlInstance<Clipboard>::set(&preferences.clipboard);
}

// ---- Creatable and uncreatable types ----

struct TerminalViewQml {
    Q_GADGET
    QML_FOREIGN(span::terminal::TerminalView)
    QML_NAMED_ELEMENT(TerminalView)
};

struct TerminalSessionQml {
    Q_GADGET
    QML_FOREIGN(span::terminal::TerminalSession)
    QML_NAMED_ELEMENT(TerminalSession)
    QML_UNCREATABLE("Terminal sessions are created by TerminalManager.")
};

struct IconMotionQml {
    Q_GADGET
    QML_FOREIGN(IconMotion)
    QML_NAMED_ELEMENT(IconMotion)
};

struct PixelSnapQml {
    Q_GADGET
    QML_FOREIGN(PixelSnap)
    QML_NAMED_ELEMENT(PixelSnap)
};

struct MarkdownDocumentRendererQml {
    Q_GADGET
    QML_FOREIGN(MarkdownDocumentRenderer)
    QML_NAMED_ELEMENT(MarkdownDocumentRenderer)
};

struct EditorControllerQml {
    Q_GADGET
    QML_FOREIGN(EditorController)
    QML_NAMED_ELEMENT(EditorController)
};

// Objects QML receives from services and passes between components: named
// so QML properties can hold them by type. Qt compiles a creation path for a
// foreign type that is default-constructible even when it is uncreatable,
// deriving from it, so such types must not be final.

struct ProjectContextQml {
    Q_GADGET
    QML_FOREIGN(span::application::ProjectContext)
    QML_NAMED_ELEMENT(ProjectContext)
    QML_UNCREATABLE("Project contexts are created by ProjectRegistry.")
};

struct FileTreeModelQml {
    Q_GADGET
    QML_FOREIGN(FileTreeModel)
    QML_NAMED_ELEMENT(FileTreeModel)
    QML_UNCREATABLE("File trees belong to a ProjectContext.")
};

struct DocumentManagerQml {
    Q_GADGET
    QML_FOREIGN(DocumentManager)
    QML_NAMED_ELEMENT(DocumentManager)
    QML_UNCREATABLE("Document managers belong to a ProjectContext.")
};

struct GitRepositoryQml {
    Q_GADGET
    QML_FOREIGN(GitRepositoryService)
    QML_NAMED_ELEMENT(GitRepository)
    QML_UNCREATABLE("Git repositories belong to a ProjectContext.")
};

struct GitItemModelQml {
    Q_GADGET
    QML_FOREIGN(GitItemModel)
    QML_NAMED_ELEMENT(GitItemModel)
    QML_UNCREATABLE("Git item models belong to a GitRepository.")
};

struct RemoteHostContextQml {
    Q_GADGET
    QML_FOREIGN(span::remote::RemoteHostContext)
    QML_NAMED_ELEMENT(RemoteHostContext)
    QML_UNCREATABLE("Remote host contexts are created by RemoteHosts.")
};

struct RemoteFileModelQml {
    Q_GADGET
    QML_FOREIGN(span::remote::RemoteFileModel)
    QML_NAMED_ELEMENT(RemoteFileModel)
    QML_UNCREATABLE("Remote file models belong to a RemoteHostContext.")
};

struct RemoteSystemMonitorQml {
    Q_GADGET
    QML_FOREIGN(span::remote::RemoteSystemMonitor)
    QML_NAMED_ELEMENT(RemoteSystemMonitor)
    QML_UNCREATABLE("Remote system monitors belong to a RemoteHostContext.")
};

// ---- Preferences ----

struct AppearanceQml {
    Q_GADGET
    QML_FOREIGN(span::application::AppearanceSettings)
    QML_NAMED_ELEMENT(Appearance)
    QML_SINGLETON
public:
    static AppearanceSettings *create(QQmlEngine *, QJSEngine *) { return QmlInstance<AppearanceSettings>::create(); }
};

struct TerminalSettingsQml {
    Q_GADGET
    QML_FOREIGN(span::application::TerminalSettings)
    QML_NAMED_ELEMENT(TerminalSettings)
    QML_SINGLETON
public:
    static TerminalSettings *create(QQmlEngine *, QJSEngine *) { return QmlInstance<TerminalSettings>::create(); }
};

struct WorkspaceSettingsQml {
    Q_GADGET
    QML_FOREIGN(span::application::WorkspaceSettings)
    QML_NAMED_ELEMENT(WorkspaceSettings)
    QML_SINGLETON
public:
    static WorkspaceSettings *create(QQmlEngine *, QJSEngine *) { return QmlInstance<WorkspaceSettings>::create(); }
};

struct ClipboardQml {
    Q_GADGET
    QML_FOREIGN(span::application::Clipboard)
    QML_NAMED_ELEMENT(Clipboard)
    QML_SINGLETON
public:
    static Clipboard *create(QQmlEngine *, QJSEngine *) { return QmlInstance<Clipboard>::create(); }
};

// ---- Services shared by the workspace and the standalone terminal ----

struct MessageCenterQml {
    Q_GADGET
    QML_FOREIGN(span::application::MessageCenter)
    QML_NAMED_ELEMENT(MessageCenter)
    QML_SINGLETON
public:
    static MessageCenter *create(QQmlEngine *, QJSEngine *) { return QmlInstance<MessageCenter>::create(); }
};

struct ShortcutsQml {
    Q_GADGET
    QML_FOREIGN(span::application::ShortcutRegistry)
    QML_NAMED_ELEMENT(Shortcuts)
    QML_SINGLETON
public:
    static ShortcutRegistry *create(QQmlEngine *, QJSEngine *) { return QmlInstance<ShortcutRegistry>::create(); }
};

struct TerminalManagerQml {
    Q_GADGET
    QML_FOREIGN(span::terminal::TerminalManager)
    QML_NAMED_ELEMENT(TerminalManager)
    QML_SINGLETON
public:
    static span::terminal::TerminalManager *create(QQmlEngine *, QJSEngine *)
    {
        return QmlInstance<span::terminal::TerminalManager>::create();
    }
};

struct UiIconsQml {
    Q_GADGET
    QML_FOREIGN(UiIconCatalog)
    QML_NAMED_ELEMENT(UiIcons)
    QML_SINGLETON
public:
    static UiIconCatalog *create(QQmlEngine *, QJSEngine *) { return QmlInstance<UiIconCatalog>::create(); }
};

struct WindowTransitionQml {
    Q_GADGET
    QML_FOREIGN(span::application::WindowTransitionCoordinator)
    QML_NAMED_ELEMENT(WindowTransition)
    QML_SINGLETON
public:
    static WindowTransitionCoordinator *create(QQmlEngine *, QJSEngine *)
    {
        return QmlInstance<WindowTransitionCoordinator>::create();
    }
};

// ---- Workspace services ----

struct ProjectRegistryQml {
    Q_GADGET
    QML_FOREIGN(span::application::ProjectWorkspaceRegistry)
    QML_NAMED_ELEMENT(ProjectRegistry)
    QML_SINGLETON
public:
    static ProjectWorkspaceRegistry *create(QQmlEngine *, QJSEngine *)
    {
        return QmlInstance<ProjectWorkspaceRegistry>::create();
    }
};

struct RemoteHostsQml {
    Q_GADGET
    QML_FOREIGN(span::remote::RemoteHostRegistry)
    QML_NAMED_ELEMENT(RemoteHosts)
    QML_SINGLETON
public:
    static span::remote::RemoteHostRegistry *create(QQmlEngine *, QJSEngine *)
    {
        return QmlInstance<span::remote::RemoteHostRegistry>::create();
    }
};

struct TailscaleDevicesQml {
    Q_GADGET
    QML_FOREIGN(span::remote::TailscaleDeviceModel)
    QML_NAMED_ELEMENT(TailscaleDevices)
    QML_SINGLETON
public:
    static span::remote::TailscaleDeviceModel *create(QQmlEngine *, QJSEngine *)
    {
        return QmlInstance<span::remote::TailscaleDeviceModel>::create();
    }
};

struct WorkspaceControllerQml {
    Q_GADGET
    QML_FOREIGN(span::application::WorkspaceController)
    QML_NAMED_ELEMENT(WorkspaceController)
    QML_SINGLETON
public:
    static WorkspaceController *create(QQmlEngine *, QJSEngine *) { return QmlInstance<WorkspaceController>::create(); }
};

struct WorkspaceLayoutQml {
    Q_GADGET
    QML_FOREIGN(span::workspace::WorkspaceLayout)
    QML_NAMED_ELEMENT(WorkspaceLayout)
    QML_SINGLETON
public:
    static span::workspace::WorkspaceLayout *create(QQmlEngine *, QJSEngine *)
    {
        return QmlInstance<span::workspace::WorkspaceLayout>::create();
    }
};

struct SessionModelQml {
    Q_GADGET
    QML_FOREIGN(SessionModel)
    QML_NAMED_ELEMENT(SessionModel)
    QML_SINGLETON
public:
    static SessionModel *create(QQmlEngine *, QJSEngine *) { return QmlInstance<SessionModel>::create(); }
};

struct ResourceMonitorQml {
    Q_GADGET
    QML_FOREIGN(ResourceMonitor)
    QML_NAMED_ELEMENT(ResourceMonitor)
    QML_SINGLETON
public:
    static ResourceMonitor *create(QQmlEngine *, QJSEngine *) { return QmlInstance<ResourceMonitor>::create(); }
};

struct AppUpdateQml {
    Q_GADGET
    QML_FOREIGN(UpdateService)
    QML_NAMED_ELEMENT(AppUpdate)
    QML_SINGLETON
public:
    static UpdateService *create(QQmlEngine *, QJSEngine *) { return QmlInstance<UpdateService>::create(); }
};

struct TaskProgressQml {
    Q_GADGET
    QML_FOREIGN(span::application::TaskProgressService)
    QML_NAMED_ELEMENT(TaskProgress)
    QML_SINGLETON
public:
    static TaskProgressService *create(QQmlEngine *, QJSEngine *) { return QmlInstance<TaskProgressService>::create(); }
};

struct ToolsQml {
    Q_GADGET
    QML_FOREIGN(span::application::ToolRegistry)
    QML_NAMED_ELEMENT(Tools)
    QML_SINGLETON
public:
    static ToolRegistry *create(QQmlEngine *, QJSEngine *) { return QmlInstance<ToolRegistry>::create(); }
};

struct ShellsQml {
    Q_GADGET
    QML_FOREIGN(span::application::ShellProfileRegistry)
    QML_NAMED_ELEMENT(Shells)
    QML_SINGLETON
public:
    static ShellProfileRegistry *create(QQmlEngine *, QJSEngine *)
    {
        return QmlInstance<ShellProfileRegistry>::create();
    }
};

struct GraphicsQml {
    Q_GADGET
    QML_FOREIGN(span::application::GraphicsModeService)
    QML_NAMED_ELEMENT(Graphics)
    QML_SINGLETON
public:
    static GraphicsModeService *create(QQmlEngine *, QJSEngine *) { return QmlInstance<GraphicsModeService>::create(); }
};

struct FileIconThemeQml {
    Q_GADGET
    QML_FOREIGN(FileIconTheme)
    QML_NAMED_ELEMENT(FileIconTheme)
    QML_SINGLETON
public:
    static FileIconTheme *create(QQmlEngine *, QJSEngine *) { return QmlInstance<FileIconTheme>::create(); }
};

struct TranslatorQml {
    Q_GADGET
    QML_FOREIGN(Translator)
    QML_NAMED_ELEMENT(Translator)
    QML_SINGLETON
public:
    static Translator *create(QQmlEngine *, QJSEngine *) { return QmlInstance<Translator>::create(); }
};

struct WindowChromeQml {
    Q_GADGET
    QML_FOREIGN(WindowChrome)
    QML_NAMED_ELEMENT(WindowChrome)
    QML_SINGLETON
public:
    static WindowChrome *create(QQmlEngine *, QJSEngine *) { return QmlInstance<WindowChrome>::create(); }
};

} // namespace span::application::runtime
