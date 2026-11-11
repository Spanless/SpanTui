#include "application/runtime/workspace_visual_harness.h"

#include "application/application_constants.h"
#include "application/preferences.h"
#include "application/project_workspace_registry.h"
#include "application/workspace_controller.h"
#include "application/runtime/internal_launch_options.h"
#include "application/runtime/qml_types.h"
#include "application/ui_icon_catalog.h"
#include "workspace/session_model.h"
#include "core/product_paths.h"
#include "platform/diagnostics/graphics_startup.h"
#include "terminal/api/terminal_manager.h"
#include "terminal/terminal_constants.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QGuiApplication>
#include <QImage>
#include <QLockFile>
#include <QQmlApplicationEngine>
#include <QQuickWindow>
#include <QStandardPaths>
#include <QTimer>
#include <QUuid>
#include <QVariantMap>

namespace span::application::runtime {

int runBoundaryGalleryApplication(
    QGuiApplication &app, const InternalLaunchOptions &options)
{
    // Never opened: the gallery reads the default preferences. Its QML
    // reaches no service but the icon catalog (qml_types.h).
    Preferences preferences;
    UiIconCatalog uiIcons;
    exposePreferences(preferences);
    QmlInstance<UiIconCatalog>::set(&uiIcons);
    QQmlApplicationEngine engine;
    QObject::connect(
        &engine, &QQmlApplicationEngine::objectCreationFailed,
        &app, []() { QCoreApplication::exit(-1); },
        Qt::QueuedConnection);
    engine.loadFromModule(kQmlModuleUri, QStringLiteral("BoundaryGallery"));
    auto *window = qobject_cast<QQuickWindow *>(
        engine.rootObjects().value(0));
    if (!window) {
        return -1;
    }
    span::platform::diagnostics::configurePipelineCache(window, QString(), false);
    configureBoundaryGalleryWindow(options, window);
    return app.exec();
}

namespace {

QString ownerLockPath(const QString &runPath)
{
    return runPath + QStringLiteral(".lock");
}

// Owner detection by process id only: a live run may hold its lock for hours.
std::unique_ptr<QLockFile> tryOwn(const QString &runPath)
{
    auto owner = std::make_unique<QLockFile>(ownerLockPath(runPath));
    owner->setStaleLockTime(0);
    if (!owner->tryLock(0)) {
        return {};
    }
    return owner;
}

void purgeAbandonedRuns(const QDir &runs)
{
    const QFileInfoList directories = runs.entryInfoList(
        {QStringLiteral("run-*")}, QDir::Dirs | QDir::NoDotAndDotDot | QDir::Hidden);
    for (const QFileInfo &directory : directories) {
        if (auto owner = tryOwn(directory.absoluteFilePath())) {
            if (!QDir(directory.absoluteFilePath()).removeRecursively()) {
                qWarning().noquote() << "Could not remove abandoned isolated run"
                                     << QDir::toNativeSeparators(directory.absoluteFilePath());
            }
        }
    }
    // Locks whose run directory is already gone: owning one removes it.
    const QFileInfoList locks = runs.entryInfoList(
        {QStringLiteral("run-*.lock")}, QDir::Files | QDir::Hidden);
    for (const QFileInfo &lock : locks) {
        const QString runPath = lock.absoluteFilePath().chopped(5);
        if (!QFileInfo::exists(runPath)) {
            tryOwn(runPath);
        }
    }
}

} // namespace

IsolatedRunData::IsolatedRunData(QString path, std::unique_ptr<QLockFile> owner)
    : m_path(std::move(path))
    , m_owner(std::move(owner))
{
}

IsolatedRunData::~IsolatedRunData()
{
    QDir(m_path).removeRecursively();
    m_owner.reset();
}

std::unique_ptr<IsolatedRunData> IsolatedRunData::create(QString *errorMessage)
{
    const QDir runs(span::core::ProductPaths::isolatedRunsDirectory());
    if (!runs.mkpath(QStringLiteral("."))) {
        if (errorMessage) {
            *errorMessage = QStringLiteral("cannot create %1")
                                .arg(QDir::toNativeSeparators(runs.absolutePath()));
        }
        return nullptr;
    }
    purgeAbandonedRuns(runs);
    // Lock first, then create: a directory without a held lock is abandoned.
    const QString path = runs.filePath(
        QStringLiteral("run-") + QUuid::createUuid().toString(QUuid::WithoutBraces));
    auto owner = tryOwn(path);
    if (!owner || !QDir().mkpath(path)) {
        if (errorMessage) {
            *errorMessage = QStringLiteral("cannot create %1")
                                .arg(QDir::toNativeSeparators(path));
        }
        return nullptr;
    }
    return std::unique_ptr<IsolatedRunData>(new IsolatedRunData(path, std::move(owner)));
}

std::unique_ptr<IsolatedRunData> isolateVisualHarnessPersistence(const InternalLaunchOptions &options,
                                                                 SettingsStore &store, QString *errorMessage)
{
    if (errorMessage) {
        errorMessage->clear();
    }
    if (!options.workspaceShot && !options.isolatedWorkspace) {
        return {};
    }
    QString reason;
    auto data = IsolatedRunData::create(&reason);
    if (!data) {
        // Fail closed: continuing would initialize the real product database
        // while the caller explicitly asked for an isolated run.
        if (errorMessage) {
            *errorMessage = QStringLiteral(
                "Could not create an isolated workspace data directory (%1); "
                "refusing to fall back to the live product database.").arg(reason);
        }
        return nullptr;
    }
    store.setDatabasePathForTesting(QDir(data->path()).filePath(QStringLiteral("workspace.db")));
    return data;
}

void applyVisualHarnessState(const InternalLaunchOptions &options, AppearanceSettings &appearance,
                             span::application::ProjectWorkspaceRegistry &projects,
                             span::application::WorkspaceController &workspaceController)
{
    if (!options.workspaceShot) {
        return;
    }
    appearance.setThemeMode(options.workspaceShotTheme);
    QString projectPath = options.workspaceShotProjectPath;
    if (projectPath.isEmpty() && !options.workspaceShotFilePath.isEmpty()) {
        projectPath = QFileInfo(options.workspaceShotFilePath).absolutePath();
    }
    if (projectPath.isEmpty()) {
        return;
    }
    projects.addProject(projectPath);
    if (!options.workspaceShotFilePath.isEmpty()) {
        workspaceController.openFile(projectPath, options.workspaceShotFilePath);
    }
    // A real ConPTY session lands in the catalog; the terminal tile is
    // created by showSession and painted by the shared harness spawner.
    if (options.workspaceShotTerminal) {
        workspaceController.createSession(
            projectPath, QStringLiteral("Terminal"), projectPath);
    }
}

void createVisualHarnessTerminals(
    const InternalLaunchOptions &options,
    const SessionModel &sessionModel,
    span::terminal::TerminalManager &terminalManager)
{
    if (!options.workspaceShot) {
        return;
    }

    QString commandPrompt = QStandardPaths::findExecutable(QStringLiteral("cmd.exe"));
    if (commandPrompt.isEmpty()) {
        commandPrompt = QStringLiteral("cmd.exe");
    }
    const QString command = QStringLiteral("\"%1\" /Q /K")
                                .arg(QDir::toNativeSeparators(commandPrompt));

    for (const SessionRecord &session : sessionModel.records()) {
        terminalManager.createSession(session.id, session.cwd, {{terminal::kSessionCommandLineKey, command}});
    }
}

void configureBoundaryGalleryWindow(
    const InternalLaunchOptions &options, QQuickWindow *window)
{
    window->setProperty("darkMode", options.boundaryGalleryTheme != kThemeLight);
    window->setVisibility(QWindow::Windowed);
    if (options.boundaryGalleryShotPath.isEmpty()) {
        return;
    }
    QTimer::singleShot(
        800, window,
        [window, path = options.boundaryGalleryShotPath]() {
            const QImage shot = window->grabWindow();
            if (shot.isNull() || !shot.save(path)) {
                qWarning().noquote()
                    << "Cannot save boundary gallery screenshot:"
                    << path;
            }
            QCoreApplication::quit();
        });
}

void configureWorkspaceShotWindow(
    const InternalLaunchOptions &options, QQuickWindow *window)
{
    window->showNormal();
    window->setGeometry(0, 0, options.workspaceShotViewport.width(),
                        options.workspaceShotViewport.height());
    window->setProperty("page", options.workspaceShotPage);
    if (!options.workspaceShotRightTool.isEmpty()) {
        window->setProperty("shotRightPage", options.workspaceShotRightTool);
    }
    QTimer::singleShot(
        options.workspaceShotDelayMs, window,
        [window, path = options.workspaceShotPath]() {
            const QImage shot = window->grabWindow();
            if (shot.isNull() || !shot.save(path)) {
                qWarning().noquote()
                    << "Cannot save workspace screenshot:" << path;
            }
            QCoreApplication::quit();
        });
}

} // namespace span::application::runtime
