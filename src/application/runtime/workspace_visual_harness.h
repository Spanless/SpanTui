#pragma once

#include <QString>

#include <memory>

class QGuiApplication;
class QLockFile;
class QQuickWindow;
class SessionModel;

namespace span::terminal {
class TerminalManager;
}

namespace span::application {
class AppearanceSettings;
class ProjectWorkspaceRegistry;
class SettingsStore;
class WorkspaceController;
}

namespace span::application::runtime {

struct InternalLaunchOptions;

int runBoundaryGalleryApplication(
    QGuiApplication &app, const InternalLaunchOptions &options);

// Scratch data of one isolated run: run-<uuid> under
// ProductPaths::isolatedRunsDirectory(), owned through run-<uuid>.lock and
// removed on destruction. A run killed before that leaves both behind; the
// next isolated run removes every directory whose owner is gone.
class IsolatedRunData final {
public:
    static std::unique_ptr<IsolatedRunData> create(QString *errorMessage);
    ~IsolatedRunData();

    IsolatedRunData(const IsolatedRunData &) = delete;
    IsolatedRunData &operator=(const IsolatedRunData &) = delete;

    QString path() const { return m_path; }

private:
    IsolatedRunData(QString path, std::unique_ptr<QLockFile> owner);

    QString m_path;
    std::unique_ptr<QLockFile> m_owner;
};

// Null without `errorMessage`: isolation does not apply. With it: refuse to
// start, or the real database would be touched.
std::unique_ptr<IsolatedRunData> isolateVisualHarnessPersistence(const InternalLaunchOptions &options,
                                                                 SettingsStore &store, QString *errorMessage = nullptr);

void applyVisualHarnessState(const InternalLaunchOptions &options, AppearanceSettings &appearance,
                             span::application::ProjectWorkspaceRegistry &projects,
                             span::application::WorkspaceController &workspaceController);

void createVisualHarnessTerminals(
    const InternalLaunchOptions &options,
    const SessionModel &sessionModel,
    span::terminal::TerminalManager &terminalManager);

void configureBoundaryGalleryWindow(
    const InternalLaunchOptions &options, QQuickWindow *window);

// Deterministic geometry, then one screenshot and exit.
void configureWorkspaceShotWindow(
    const InternalLaunchOptions &options, QQuickWindow *window);

} // namespace span::application::runtime
