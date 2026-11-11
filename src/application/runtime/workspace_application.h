#pragma once

class QGuiApplication;

namespace span::application {
struct LaunchOptions;
}

namespace span::application::runtime {

struct InternalLaunchOptions;

int runWorkspaceApplication(QGuiApplication &app,
                            const LaunchOptions &launchOptions,
                            const InternalLaunchOptions &internalOptions);

} // namespace span::application::runtime
