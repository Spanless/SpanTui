#include "application/runtime/workspace_runtime.h"

#include "application/application_constants.h"
#include "application/diagnostics/resource_reporter.h"
#include "application/diagnostics/ui_health_monitor.h"
#include "application/launch_options.h"
#include "application/message_center.h"
#include "application/project_context.h"
#include "application/runtime/internal_launch_options.h"
#include "application/runtime/qml_types.h"
#include "application/runtime/workspace_visual_harness.h"
#include "application/session_terminal_routing.h"
#include "application/terminal_host_events.h"
#include "application/terminal_resource_targets.h"
#include "application/translator.h"
#include "application/workspace_messages.h"
#include "core/product_paths.h"
#include "documents/document_manager.h"
#include "git/git_repository_service.h"
#include "platform/platform_constants.h"
#include "platform/windows/program_icon_provider.h"
#include "platform/windows/window_chrome_geometry.h"
#include "remote/remote_file_model.h"
#include "remote/remote_host_context.h"
#include "remote/remote_transfer_queue.h"
#include "terminal/core/terminal_color.h"
#include "terminal/terminal_constants.h"
#include "workspace/file_tree_model.h"
#include "workspace/scope.h"

#include <QGuiApplication>
#include <QQmlApplicationEngine>
#include <QQmlError>
#include <QQuickWindow>
#include <QScreen>
#include <QStyleHints>
#include <QTimer>

namespace span::application::runtime {
namespace {

using span::workspace::Scope;

constexpr int kStartupUpdateCheckDelayMs = 1500;

QRect virtualDesktopGeometry()
{
    QRect combined;
    const auto screens = QGuiApplication::screens();
    for (const QScreen *screen : screens) {
        combined |= screen->geometry();
    }
    return combined;
}

void postToMessageCenter(MessageKind kind, const QString &message)
{
    if (auto *center = MessageCenter::instance()) {
        center->post(kind, message);
    }
}

// One path for service errors: the logging handler files a critical as one
// Error message.
template <typename Source> void reportErrors(Source *source)
{
    QObject::connect(source, &Source::errorOccurred, source,
                     [](const QString &message) { qCritical().noquote() << "Application error:" << message; });
}

} // namespace

WorkspaceRuntime::WorkspaceRuntime(QGuiApplication &app, const LaunchOptions &launchOptions,
                                   const InternalLaunchOptions &internal)
    : m_app(app),
      m_launchOptions(launchOptions),
      m_internal(internal),
      m_shortcuts(&m_preferences.workspace),
      m_resourceMonitor([this] { return terminalResourceTargets(m_terminalManager); })
{
}

WorkspaceRuntime::~WorkspaceRuntime()
{
    // QML first: its bindings and handlers may still read context objects
    // (windowChrome, windowTransition, translator, ...) while being destroyed.
    m_uiHealthMonitor.reset();
    m_engine.reset();
    m_translator.reset();
}

int WorkspaceRuntime::initialize()
{
    const int persistence = initializePersistence();
    if (persistence != 0) {
        return persistence;
    }
    prepareGraphics();
    wireLayout();
    wireContexts();
    // After wireContexts(): the project context the harness opens gets wired.
    applyVisualHarnessState(m_internal, m_preferences.appearance, *m_projectRegistry, *m_workspaceController);
    wireAppearance();
    wireSessions();
    wireLifecycle();
    if (!loadQml()) {
        qCritical("Workspace root window missing after QML load.");
        return -1;
    }
    attachWindow();
    return 0;
}

int WorkspaceRuntime::initializePersistence()
{
    QString isolationError;
    m_visualHarnessData = isolateVisualHarnessPersistence(m_internal, m_preferences.store, &isolationError);
    if (!isolationError.isEmpty()) {
        qCritical().noquote() << isolationError;
        return 6;
    }

    wireErrors();
    m_preferences.store.setSessionModel(&m_sessionModel);
    if (!m_preferences.open(SettingsStore::Mode::Workspace)) {
        qCritical("Cannot initialize workspace persistence.");
        return 5;
    }
    const QString databasePath = m_preferences.store.databasePath();
    if (auto *messages = MessageCenter::instance()) messages->initializePersistence(databasePath);
    // File-tree undo lives only within one process: leftover trash is
    // unrestorable.
    FileTreeModel::purgeTrash(FileTreeModel::defaultTrashRoot());

    // Tools resolve before any project context exists, so no Git command
    // starts against an executable the user has overridden or that is missing.
    m_tools = std::make_unique<ToolRegistry>(&m_preferences.workspace);
    m_tools->refresh();
    m_shells = std::make_unique<ShellProfileRegistry>(&m_preferences.terminal);
    m_projectRegistry = std::make_unique<ProjectWorkspaceRegistry>(databasePath);
    m_projectRegistry->setGitEnabled(m_tools->gitAvailable());
    QObject::connect(m_tools.get(), &ToolRegistry::executableChanged,
                     m_projectRegistry.get(), [this](span::core::ExternalTool tool) {
        if (tool != span::core::ExternalTool::Git) {
            return;
        }
        // Git Bash is found next to the Git executable.
        m_shells->refresh();
        // A different Git (or none) invalidates every repository read:
        // drop them and discover again with the new executable.
        m_projectRegistry->setGitEnabled(false);
        m_projectRegistry->setGitEnabled(m_tools->gitAvailable());
    });
    // A device list read with another (or no) tailscale CLI is stale; one
    // nobody asked for yet stays unread.
    QObject::connect(
        m_tools.get(), &ToolRegistry::executableChanged, &m_tailscaleDevices, [this](span::core::ExternalTool tool) {
            if (tool == span::core::ExternalTool::Tailscale && m_tailscaleDevices.state() != QLatin1String("idle")) {
                m_tailscaleDevices.refresh();
            }
        });
    // IdleGovernor owns the "user idle" fact (window inactive or 30 s
    // without keyboard/mouse input); pollers back off while it holds.
    m_idleGovernor.attach(&m_app);
    const auto applyIdle = [this]() {
        m_resourceMonitor.setIdle(m_idleGovernor.idle());
        m_projectRegistry->setIdle(m_idleGovernor.idle());
    };
    QObject::connect(&m_idleGovernor, &IdleGovernor::idleChanged,
                     m_projectRegistry.get(), applyIdle);
    applyIdle();
    // Without the askpass pipe ssh keeps its own console prompts: remote
    // terminals still work, the remote file browser then needs key login.
    QString askpassError;
    if (!m_askpassBroker.listen(&askpassError)) {
        qWarning().noquote() << "SSH prompt bridge unavailable:" << askpassError;
    }
    m_remoteHosts = std::make_unique<span::remote::RemoteHostRegistry>(databasePath, &m_askpassBroker);
    m_workspaceController = std::make_unique<WorkspaceController>(
        m_projectRegistry.get(), &m_workspaceLayout, &m_sessionModel,
        &m_terminalManager, m_remoteHosts.get());
    reportErrors(m_projectRegistry.get());
    reportErrors(m_remoteHosts.get());
    reportErrors(m_workspaceController.get());
    return 0;
}

void WorkspaceRuntime::wireErrors()
{
    reportErrors(&m_preferences.store);
    reportErrors(&m_preferences.terminal);
    reportErrors(&m_terminalManager);
    if (auto *messages = MessageCenter::instance()) {
        QObject::connect(&m_updateService,
                         &UpdateService::updateFailed, messages,
                         [](const QString &message) {
                             postToMessageCenter(MessageKind::Error, message);
                         });
    }
}

void WorkspaceRuntime::wireLayout()
{
    m_workspaceLayout.restoreFromJson(m_preferences.store.workspaceLayoutJson());
    // A session whose saved host is gone cannot reconnect; drop it before
    // the layout and the terminal manager see the catalog. When the host
    // list could not be read, every remote session and its tiles stay.
    const bool hostsKnown = m_remoteHosts->loaded();
    if (hostsKnown) {
        for (const SessionRecord &session : m_sessionModel.records()) {
            const Scope scope = Scope::fromKey(session.projectPath);
            if (scope.isRemoteHost() && !m_remoteHosts->contains(scope.hostId())) {
                m_sessionModel.removeSessionById(session.id);
            }
        }
    }
    QStringList scopes = m_projectRegistry->projectPaths();
    for (const QString &hostId : m_remoteHosts->hostIds()) {
        scopes << Scope::remoteHost(hostId).key();
    }
    QHash<QString, QString> sessionScopes;
    for (const SessionRecord &session : m_sessionModel.records()) {
        const QString &scope = session.projectPath;
        sessionScopes.insert(session.id, scope);
        if (!hostsKnown && Scope::fromKey(scope).isRemoteHost() && !scopes.contains(scope)) {
            scopes << scope;
        }
    }
    m_workspaceLayout.reconcileReferences(scopes, sessionScopes);
    const auto persistLayout = [this]() { m_preferences.store.setWorkspaceLayoutJson(m_workspaceLayout.toJson()); };
    QObject::connect(&m_workspaceLayout, &span::workspace::WorkspaceLayout::tilesChanged, &m_preferences.store,
                     persistLayout);
    QObject::connect(&m_workspaceLayout, &span::workspace::WorkspaceLayout::layoutCommitted, &m_preferences.store,
                     persistLayout);
    if (!m_launchOptions.projectPath.isEmpty()) {
        m_projectRegistry->addProject(m_launchOptions.projectPath);
    }
}

// Progress and messages of the models a context owns; see
// docs/services/application/messages-task-progress.md.
void WorkspaceRuntime::wireProjectContext(ProjectContext *context)
{
    bindProgress(&m_taskProgress, context->fileTree(), ProgressOwner::Files);
    bindGitProgress(&m_taskProgress, context->gitRepository(), context);
    connectFileTreeMessages(context->fileTree(), context);
    connectGitMessages(context->gitRepository(), context);
    QObject::connect(context, &ProjectContext::editorDocumentsCreated, context, [this](DocumentManager *documents) {
        bindProgress(&m_taskProgress, documents, ProgressOwner::Document);
        connectDocumentMessages(documents);
    });
}

void WorkspaceRuntime::wireRemoteContext(span::remote::RemoteHostContext *context)
{
    auto *files = context->fileModel();
    bindProgress(&m_taskProgress, files->transfers(), ProgressOwner::Remote);
    connectRemoteFileMessages(files, context);
}

void WorkspaceRuntime::wireContexts()
{
    QObject::connect(m_projectRegistry.get(), &ProjectWorkspaceRegistry::contextCreated, m_projectRegistry.get(),
                     [this](ProjectContext *context) { wireProjectContext(context); });
    QObject::connect(m_remoteHosts.get(), &span::remote::RemoteHostRegistry::contextCreated, m_remoteHosts.get(),
                     [this](span::remote::RemoteHostContext *context) { wireRemoteContext(context); });
    bindProgress(&m_taskProgress, &m_updateService, ProgressOwner::Update);
}

void WorkspaceRuntime::wireAppearance()
{
    AppearanceSettings &appearance = m_preferences.appearance;
    QStyleHints *hints = m_app.styleHints();
    appearance.setSystemDark(hints->colorScheme() == Qt::ColorScheme::Dark);
    appearance.setSystemWheelScrollLines(hints->wheelScrollLines());
    appearance.refreshSystemReduceMotion();
    m_fileIconTheme.setKnownNames(m_uiIcons.names());
    QObject::connect(hints, &QStyleHints::colorSchemeChanged, &appearance, [&appearance](Qt::ColorScheme scheme) {
        appearance.setSystemDark(scheme == Qt::ColorScheme::Dark);
    });
    QObject::connect(hints, &QStyleHints::wheelScrollLinesChanged, &appearance,
                     &AppearanceSettings::setSystemWheelScrollLines);

    const auto applyTerminalSurfaceColors = [this]() {
        const span::terminal::TerminalColorScheme scheme = m_preferences.terminal.activeColorScheme();
        m_terminalManager.setSurfaceColors(
            scheme.foreground, scheme.background, scheme.ansi);
    };
    applyTerminalSurfaceColors();
    QObject::connect(&m_preferences.terminal, &TerminalSettings::appearanceChanged, &m_terminalManager,
                     applyTerminalSurfaceColors);

    const auto applyApplicationFont = [&appearance]() {
        QGuiApplication::setFont(appearance.uiFont(appearance.uiFontSize()));
    };
    applyApplicationFont();
    QObject::connect(&appearance, &AppearanceSettings::appearanceChanged, &appearance, applyApplicationFont);
    const auto applyMonitorInterval = [this]() {
        m_resourceMonitor.setIntervalSeconds(m_preferences.workspace.performanceMonitorIntervalSeconds());
    };
    applyMonitorInterval();
    QObject::connect(&m_preferences.workspace, &WorkspaceSettings::performanceMonitorIntervalChanged,
                     &m_resourceMonitor, applyMonitorInterval);
}

void WorkspaceRuntime::wireSessions()
{
    // Harness terminals exist before the first synchronization, so it keeps
    // them instead of launching the sessions' own shells.
    createVisualHarnessTerminals(m_internal, m_sessionModel, m_terminalManager);
    connectSessionTerminals(&m_sessionModel, &m_terminalManager, m_remoteHosts.get(),
                            m_shells.get());
}

void WorkspaceRuntime::prepareGraphics()
{
    // Automation runs never show a window, so they neither leave nor read
    // the marker of a real launch.
    const bool automation = m_internal.headless || m_internal.isolatedVisualHarness();
    m_graphicsWatch = std::make_unique<span::platform::diagnostics::GraphicsStartupWatch>(
        automation ? QString() : span::core::ProductPaths::graphicsStartMarkerPath());
    m_graphics = std::make_unique<GraphicsModeService>(&m_preferences.workspace, m_launchOptions.safeGraphics,
                                                       m_graphicsWatch->previousStartInterrupted());
    if (m_graphics->softwareActive()) {
        qInfo().noquote() << "Graphics: software rendering requested ("
                                 + m_graphics->softwareReason() + ")";
        span::platform::diagnostics::preferSoftwareRendering();
    }
    m_graphicsWatch->begin();
}

void WorkspaceRuntime::wireLifecycle()
{
    QObject::connect(&m_app, &QCoreApplication::aboutToQuit, &m_preferences.store, &SettingsStore::saveNow);
}

bool WorkspaceRuntime::loadQml()
{
    QQuickWindow::setTextRenderType(QQuickWindow::QtTextRendering);
    m_engine = std::make_unique<QQmlApplicationEngine>();
    m_translator = std::make_unique<Translator>(&m_preferences.appearance, m_engine.get());
    reportErrors(m_translator.get());
    // Shell icons come from the programs themselves.
    m_engine->addImageProvider(QString::fromLatin1(platform::kProgramIconProviderId), new ProgramIconProvider);

    // Every service QML reaches (qml_types.h), before the first component.
    exposePreferences(m_preferences);
    QmlInstance<ProjectWorkspaceRegistry>::set(m_projectRegistry.get());
    QmlInstance<span::remote::RemoteHostRegistry>::set(m_remoteHosts.get());
    QmlInstance<span::remote::TailscaleDeviceModel>::set(&m_tailscaleDevices);
    QmlInstance<WorkspaceController>::set(m_workspaceController.get());
    QmlInstance<ShortcutRegistry>::set(&m_shortcuts);
    QmlInstance<span::workspace::WorkspaceLayout>::set(&m_workspaceLayout);
    QmlInstance<MessageCenter>::set(MessageCenter::instance());
    QmlInstance<SessionModel>::set(&m_sessionModel);
    QmlInstance<span::terminal::TerminalManager>::set(&m_terminalManager);
    QmlInstance<ResourceMonitor>::set(&m_resourceMonitor);
    QmlInstance<UpdateService>::set(&m_updateService);
    QmlInstance<TaskProgressService>::set(&m_taskProgress);
    QmlInstance<ToolRegistry>::set(m_tools.get());
    QmlInstance<ShellProfileRegistry>::set(m_shells.get());
    QmlInstance<GraphicsModeService>::set(m_graphics.get());
    QmlInstance<FileIconTheme>::set(&m_fileIconTheme);
    QmlInstance<UiIconCatalog>::set(&m_uiIcons);
    QmlInstance<Translator>::set(m_translator.get());
    QmlInstance<WindowChrome>::set(&m_windowChrome);
    QmlInstance<WindowTransitionCoordinator>::set(&m_windowTransition);

    connectTerminalHostEvents(&m_terminalManager, &m_taskProgress);
    const auto retranslateServices = [this]() {
        m_sessionModel.retranslate();
        m_shells->retranslate();
        m_updateService.retranslate();
    };
    QObject::connect(m_translator.get(), &Translator::languageChanged,
                     m_translator.get(), retranslateServices);
    retranslateServices();

    QObject::connect(
        m_engine.get(), &QQmlApplicationEngine::objectCreationFailed,
        &m_app, []() {
            qCritical(
                "QQmlApplicationEngine failed to create root "
                "object (see preceding QML errors).");
            QCoreApplication::exit(-1);
        },
        Qt::QueuedConnection);
    QObject::connect(
        m_engine.get(), &QQmlEngine::warnings, m_engine.get(),
        [](const QList<QQmlError> &warnings) {
            for (const QQmlError &error : warnings) {
                qWarning().noquote() << error.toString();
            }
        });

    m_engine->loadFromModule(kQmlModuleUri, QStringLiteral("Main"));

    if (!m_internal.headless && !m_internal.workspaceShot) {
        QTimer::singleShot(kStartupUpdateCheckDelayMs, &m_updateService, [this]() {
            if (m_updateService.canCheck()) {
                m_updateService.checkNow(m_updateService.autoApply(), false);
            }
        });
    }

    m_window = qobject_cast<QQuickWindow *>(m_engine->rootObjects().value(0));
    // The single workspace window owns the cache file; Main.qml starts hidden,
    // so this lands before the first expose.
    span::platform::diagnostics::configurePipelineCache(
        m_window, span::core::ProductPaths::pipelineCachePath(), true);
    return m_window != nullptr;
}

void WorkspaceRuntime::attachWindow()
{
    QObject::connect(&m_windowTransition, &WindowTransitionCoordinator::viewportPreviewed,
                     &m_workspaceLayout, &span::workspace::WorkspaceLayout::previewViewport);
    QObject::connect(&m_windowTransition, &WindowTransitionCoordinator::epochChanged, &m_workspaceLayout,
                     &span::workspace::WorkspaceLayout::beginViewportTransaction);
    QObject::connect(&m_windowTransition, &WindowTransitionCoordinator::viewportCommitted, &m_workspaceLayout,
                     &span::workspace::WorkspaceLayout::commitViewport);
    QObject::connect(&m_windowChrome, &WindowChrome::interactiveResizeStarted,
                     &m_windowTransition, &WindowTransitionCoordinator::beginNativeResize);
    QObject::connect(&m_windowChrome, &WindowChrome::interactiveResizeEnded,
                     &m_windowTransition, &WindowTransitionCoordinator::endNativeResize);
    QObject::connect(&m_windowChrome, &WindowChrome::nativeWindowStateChanged,
                     &m_windowTransition, &WindowTransitionCoordinator::noteNativeStateChange);
    QObject::connect(&m_windowChrome, &WindowChrome::devicePixelRatioChanged,
                     &m_windowTransition, &WindowTransitionCoordinator::noteDpiChange);
    QObject::connect(m_graphicsWatch.get(),
                     &span::platform::diagnostics::GraphicsStartupWatch::rendererReady,
                     m_graphics.get(), [this]() {
                         const auto renderer = m_graphicsWatch->renderer();
                         m_graphics->setRenderer(renderer.api, renderer.device, renderer.software);
                     });
    m_graphicsWatch->watch(m_window, m_graphics->softwareActive());
    m_uiHealthMonitor = std::make_unique<diagnostics::UiHealthMonitor>(
        &m_windowTransition, &m_terminalManager, &m_resourceMonitor, &m_preferences.store, m_window);
    m_uiHealthMonitor->start();
}

void WorkspaceRuntime::present(bool raiseForActivation)
{
    QQuickWindow *window = m_window;
    if (m_internal.headless) {
        qInfo().noquote()
            << "SpanTui headless mode: root window created, not shown.";
        window->setVisibility(QWindow::Hidden);
        m_windowTransition.attachWindow(window);
    } else {
        m_windowChrome.attach(window);
        m_windowChrome.setDarkFrame(m_preferences.appearance.effectiveDark());

        const QRect restored = window_chrome_geometry::clampToVirtualDesktop(
            m_preferences.store.windowGeometry(), virtualDesktopGeometry(),
            QSize(window->minimumWidth(), window->minimumHeight()));
        if (restored.isValid()) {
            window->setGeometry(restored);
        }
        m_windowTransition.attachWindow(window);
        if (m_internal.workspaceShot) {
            configureWorkspaceShotWindow(m_internal, window);
        } else if (m_preferences.store.windowMaximized()) {
            window->showMaximized();
        } else {
            window->show();
        }

        // Reuse the live raise/activate path for activations that arrived
        // before the window existed (including empty-path "focus primary").
        if (raiseForActivation) {
            window->raise();
            window->requestActivate();
        }

        QObject::connect(&m_app, &QCoreApplication::aboutToQuit, &m_preferences.store, [this]() {
            const QRect geometry = m_windowChrome.normalGeometry();
            m_preferences.store.saveWindowGeometry(geometry.x(), geometry.y(), geometry.width(), geometry.height(),
                                                   m_windowChrome.effectivelyMaximized());
        });
        // Re-sample OS animation preference when the user returns to the app
        // (e.g. after toggling Accessibility → Animation effects).
        QObject::connect(window, &QWindow::activeChanged, &m_preferences.appearance, [this, window]() {
            if (window->isActive()) {
                m_preferences.appearance.refreshSystemReduceMotion();
            }
        });
    }
    const QString graphicsNotice = m_graphics->fallbackNotice();
    if (!graphicsNotice.isEmpty()) {
        postToMessageCenter(MessageKind::Warning, graphicsNotice);
    }
    startResourceReport();
}

void WorkspaceRuntime::startResourceReport()
{
    if (m_internal.resourceReportPath.isEmpty()) {
        return;
    }
    FileTreeModel *reportFiles = nullptr;
    const QStringList projects = m_projectRegistry->projectPaths();
    if (!projects.isEmpty()) {
        ProjectContext *context = m_projectRegistry->contextForProject(projects.constFirst());
        reportFiles = context ? context->fileTree() : nullptr;
    }
    auto *reporter = new diagnostics::ResourceReporter(
        &m_terminalManager, reportFiles, &m_app);
    QString error;
    if (!reporter->start(m_internal.resourceReportPath, m_window, &error)) {
        qWarning().noquote() << "Cannot write resource report:" << error;
        reporter->deleteLater();
    }
}

} // namespace span::application::runtime
