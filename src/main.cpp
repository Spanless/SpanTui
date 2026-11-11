#include "application/gui_application.h"
#include "application/launch_options.h"
#include "application/message_center.h"
#include "core/core_constants.h"
#include "core/product_identity.h"
#include "core/product_paths.h"
#include "application/runtime/internal_launch_options.h"
#include "application/runtime/terminal_application.h"
#include "application/runtime/workspace_application.h"
#include "platform/diagnostics/launch_logging.h"
#include "platform/windows/crash_handler.h"
#include "platform/windows/native_dialog.h"
#include "platform/windows/process_containment.h"
#include "remote/ssh_askpass.h"
#include "terminal/core/terminal_viewport_trace.h"
#include "terminal/platform/windows/win_conpty_process.h"

#include <QCoreApplication>
#include <QDir>
#include <QFontDatabase>
#include <QGuiApplication>
#include <QIcon>
#include <QQuickStyle>

int main(int argc, char *argv[])
{
    // SSH_ASKPASS helper: answered before any GUI, logging or data-root setup.
    if (span::remote::isAskpassInvocation(argc)) {
        return span::remote::runAskpassClient();
    }

    span::application::GuiApplication app(argc, argv);
    QGuiApplication::setOrganizationName(QStringLiteral("Span"));
    // The display name follows the image name, so SpanTui-test and
    // SpanTui-pre stay distinguishable.
    QGuiApplication::setApplicationName(span::core::kProductName);
    QGuiApplication::setApplicationDisplayName(
        span::core::productImageName());
    QGuiApplication::setApplicationVersion(QStringLiteral(SPANTUI_VERSION));

    const QStringList arguments = QCoreApplication::arguments();
    const span::application::LaunchOptions launchOptions =
        span::application::parseLaunchOptions(arguments);
    const auto internalOptions =
        span::application::runtime::parseInternalLaunchOptions(
            arguments);
    // A GUI-subsystem process has no console, so a failed start needs a
    // dialog; automation and offscreen runs must never block on one.
    const QString platform = QGuiApplication::platformName();
    const bool failureDialogs = !internalOptions.headless
        && !internalOptions.isolatedVisualHarness()
        && platform != QLatin1String("offscreen")
        && platform != QLatin1String("minimal");
    // Before any side effect: invalid arguments must not touch the data root.
    if (!launchOptions.valid()) {
        qCritical().noquote() << launchOptions.error;
        if (failureDialogs) {
            span::platform::dialogs::showError(
                QGuiApplication::applicationDisplayName(), launchOptions.error);
        }
        return 2;
    }
    span::core::ProductPaths::setRootOverride(launchOptions.dataDir);
    span::terminal::WinConPtyProcess::setPreferSystemConPty(internalOptions.systemConPty);
    if (internalOptions.traceViewport) {
        span::terminal::TerminalViewportTrace::setEnabled(*internalOptions.traceViewport);
    }

    // Before the crash handler and launch log create the root.
    const QString legacyRootWarning = span::core::ProductPaths::moveLegacyDefaultRoot();
    crash_handler::install(span::core::ProductPaths::crashLogDirectory());
    span::platform::diagnostics::installLaunchLogging();
    span::platform::diagnostics::setLaunchFailureDialogsEnabled(failureDialogs);
    span::application::MessageCenter messageCenter;
    messageCenter.attachToQtLogging();
    if (!legacyRootWarning.isEmpty()) {
        qWarning().noquote() << legacyRootWarning;
    }

    span::core::ProductPaths::installApplicationSettings();

    Q_INIT_RESOURCE(spantui_brand);
    Q_INIT_RESOURCE(spantui_fonts);
    Q_INIT_RESOURCE(ui_icons);
    QGuiApplication::setWindowIcon(
        QIcon(QStringLiteral(":/assets/brand/spantui.ico")));
    QQuickStyle::setStyle(QStringLiteral("Basic"));
    const QDir bundledFonts(QStringLiteral(":/fonts"));
    for (const QString &face : bundledFonts.entryList({QStringLiteral("*.ttf")}, QDir::Files)) {
        const QString path = bundledFonts.filePath(face);
        if (QFontDatabase::addApplicationFont(path) < 0) {
            qWarning().noquote() << "Failed to register bundled font:" << path;
        }
    }

    span::platform::process::installApplicationContainment();

    // QML types and services are registered declaratively
    // (application/runtime/qml_types.h).
    const int exitCode = launchOptions.terminalMode
        ? span::application::runtime::runTerminalApplication(
              app, launchOptions, internalOptions.headless)
        : span::application::runtime::runWorkspaceApplication(
              app, launchOptions, internalOptions);
    // Non-zero means the start never reached a usable window; the reason is
    // already in the launch log.
    if (exitCode != 0) {
        span::platform::diagnostics::reportLaunchFailure(
            span::platform::diagnostics::LaunchFailure::Start);
    }
    return exitCode;
}
