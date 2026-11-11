#include "application/runtime/workspace_application.h"

#include "application/launch_options.h"
#include "application/project_workspace_registry.h"
#include "application/runtime/internal_launch_options.h"
#include "application/runtime/workspace_runtime.h"
#include "application/runtime/workspace_visual_harness.h"
#include "application/single_instance_router.h"

#include <QGuiApplication>
#include <QQuickWindow>

#include <memory>
#include <utility>

namespace span::application::runtime {

int runWorkspaceApplication(QGuiApplication &app,
                            const LaunchOptions &launchOptions,
                            const InternalLaunchOptions &internal)
{
    if (internal.boundaryGallery) {
        return runBoundaryGalleryApplication(app, internal);
    }

    std::unique_ptr<SingleInstanceRouter> instanceRouter;
    ProjectWorkspaceRegistry *routingProjects = nullptr;
    QQuickWindow *routingWindow = nullptr;
    QStringList pendingActivations;
    const bool headlessActivation = internal.headless;
    const auto applyActivation =
        [&routingProjects, &routingWindow, &pendingActivations,
         headlessActivation](const QString &projectPath) {
            if (!routingProjects || !routingWindow) {
                pendingActivations.push_back(projectPath);
                return;
            }
            if (!projectPath.isEmpty()) {
                routingProjects->addProject(projectPath);
            }
            // Headless / automation keeps the window hidden but still applies
            // the activation to the project registry.
            if (headlessActivation) {
                return;
            }
            routingWindow->show();
            routingWindow->raise();
            routingWindow->requestActivate();
        };
    if (!internal.isolatedVisualHarness()) {
        instanceRouter = std::make_unique<SingleInstanceRouter>();
        QObject::connect(
            instanceRouter.get(),
            &SingleInstanceRouter::activationRequested,
            &app, applyActivation);
        QString routeError;
        const auto route = instanceRouter->startOrForward(
            launchOptions.projectPath, &routeError);
        if (route == SingleInstanceRouter::StartResult::Forwarded) {
            // Same image channel already owns the workspace; this process exits
            // after activating it. A different PE name (SpanTui-test vs SpanTui)
            // uses a separate endpoint and never takes this path.
            qInfo().noquote()
                << "Activation forwarded to the existing"
                << instanceRouter->endpointName()
                << "instance.";
            return 0;
        }
        if (route == SingleInstanceRouter::StartResult::Rejected) {
            qCritical().noquote()
                << "Primary instance rejected activation:" << routeError;
            return 3;
        }
        if (route == SingleInstanceRouter::StartResult::Failed) {
            qCritical().noquote()
                << "Cannot establish SpanTui single instance:" << routeError;
            return 3;
        }
    }

    // Declared after the router: its activation handler holds pointers into
    // the runtime, which is torn down first.
    WorkspaceRuntime runtime(app, launchOptions, internal);
    const int initialized = runtime.initialize();
    if (initialized != 0) {
        return initialized;
    }

    bool raiseForActivation = false;
    if (instanceRouter) {
        // Arm routing before visibility policy so live activations use the
        // same path as the deferred queue flush below.
        routingProjects = runtime.projectRegistry();
        routingWindow = runtime.window();
        const QStringList queuedActivations = std::exchange(pendingActivations, {});
        for (const QString &projectPath : queuedActivations) {
            if (!projectPath.isEmpty()) {
                routingProjects->addProject(projectPath);
            }
        }
        raiseForActivation = !queuedActivations.isEmpty();
    }
    runtime.present(raiseForActivation);

    return app.exec();
}

} // namespace span::application::runtime
