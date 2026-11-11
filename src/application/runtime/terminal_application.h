#pragma once

class QGuiApplication;

namespace span::application {
struct LaunchOptions;
}

namespace span::application::runtime {

int runTerminalApplication(QGuiApplication &app,
                           const LaunchOptions &options,
                           bool headless);

} // namespace span::application::runtime
