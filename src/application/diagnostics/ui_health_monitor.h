#pragma once

#include <QObject>
#include <QTimer>

#include <atomic>
#include <memory>
#include <thread>

class QQuickWindow;
class ResourceMonitor;

namespace span::application {
class SettingsStore;
class WindowTransitionCoordinator;
}


namespace span::terminal {
class TerminalManager;
}

namespace span::application::diagnostics {

// A bounded, lock-free snapshot of GUI health. The watchdog thread reads only
// atomics and precomputed paths; it never calls Qt GUI/QML objects. The
// 250 ms heartbeat slows down 5x only while the window is minimized.
class UiHealthMonitor final : public QObject {
    Q_OBJECT

public:
    UiHealthMonitor(span::application::WindowTransitionCoordinator *transition,
                    span::terminal::TerminalManager *terminals, ResourceMonitor *resources,
                    span::application::SettingsStore *settings, QQuickWindow *window,
                    const QString &diagnosticDirectory = {}, int stallThresholdMs = 2000, QObject *parent = nullptr);

    ~UiHealthMonitor() override;

    void start();

private:
    struct Snapshot;

    void pulse();
    void applyHeartbeatInterval();
    static void recordRing(Snapshot *snapshot, quint8 eventCode, quint16 detail);
    static void writeStallReport(const Snapshot &snapshot, quint64 stallEpoch,
                                 quint64 lagMs);
    static void writeRecoveredRecord(const Snapshot &snapshot,
                                     quint64 stallEpoch, quint64 lagMs);
    static void watchdogLoop(const std::shared_ptr<Snapshot> &snapshot);


    span::application::WindowTransitionCoordinator *m_transition = nullptr;
    span::terminal::TerminalManager *m_terminals = nullptr;
    ResourceMonitor *m_resources = nullptr;
    span::application::SettingsStore *m_settings = nullptr;

    QQuickWindow *m_window = nullptr;
    QTimer m_heartbeatTimer;
    std::thread m_watchdog;
    std::shared_ptr<Snapshot> m_snapshot;
    std::atomic_bool m_started{false};
};

} // namespace span::application::diagnostics
