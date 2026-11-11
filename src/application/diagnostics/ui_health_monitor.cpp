#include "application/diagnostics/ui_health_monitor.h"

#include "core/product_paths.h"
#include "application/settings_store.h"
#include "application/window_transition_coordinator.h"
#include "monitoring/resource_monitor.h"
#include "terminal/api/terminal_manager.h"
#include "terminal/api/terminal_session.h"

#include <QDir>
#include <QQuickWindow>

#include <algorithm>
#include <array>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <mutex>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

namespace span::application::diagnostics {
namespace {

constexpr int kHeartbeatMs = 250;
constexpr int kWatchdogPollMs = 250;
// While minimized the heartbeat backs off by the same factor as idle
// background work (IdleGovernor consumers); the watchdog widens its stall
// threshold by the longer gap so the slower pulse is not read as a stall.
constexpr int kMinimizedBackoff = 5;

quint64 monotonicMilliseconds()
{
    return GetTickCount64();
}

template <size_t N>
void storePath(const QString &path, std::array<wchar_t, N> *destination)
{
    destination->fill(L'\0');
    const std::wstring native = QDir::toNativeSeparators(path).toStdWString();
    const size_t count = std::min(native.size(), N - 1);
    std::copy_n(native.data(), count, destination->data());
}

void writeAll(HANDLE file, const char *text, size_t length)
{
    if (file == INVALID_HANDLE_VALUE || !text || length == 0) {
        return;
    }
    DWORD written = 0;
    WriteFile(file, text, static_cast<DWORD>(length), &written, nullptr);
}

} // namespace

struct UiHealthMonitor::Snapshot {
    std::mutex wakeMutex;
    std::condition_variable wake;
    std::atomic_bool stopping{false};
    std::atomic<quint64> lastHeartbeatMs{0};
    std::atomic<quint64> heartbeatSerial{0};
    std::atomic<int> heartbeatSlackMs{0};
    std::atomic<quint32> guiThreadId{0};
    std::atomic<quint64> transitionEpoch{0};
    std::atomic<quint64> visualCommitRevision{0};
    std::atomic<int> transitionPhase{0};
    std::atomic<int> windowState{0};
    std::atomic<int> windowWidth{0};
    std::atomic<int> windowHeight{0};
    std::atomic<int> sessionCount{0};
    std::atomic<qint64> pendingInputBytes{0};
    std::atomic<qint64> pendingOutputBytes{0};
    std::atomic<int> resourceSampleDepth{0};
    std::atomic<int> resourcePdhStatus{0};
    std::atomic<int> persistenceQueueDepth{0};
    std::atomic<unsigned> ringWrite{0};
    std::array<std::atomic<quint64>, 32> ring{};
    std::array<wchar_t, 1024> logPath{};
    int stallThresholdMs = 2000;
};

void UiHealthMonitor::recordRing(Snapshot *snapshot,
                                 quint8 eventCode, quint16 detail)
{
    if (!snapshot) {
        return;
    }
    const quint64 packed = (monotonicMilliseconds() << 24)
        | (static_cast<quint64>(eventCode) << 16)
        | static_cast<quint64>(detail);
    const unsigned write = snapshot->ringWrite.load(std::memory_order_relaxed);
    const unsigned index = write
        % static_cast<unsigned>(snapshot->ring.size());
    snapshot->ring[index].store(packed, std::memory_order_release);
    snapshot->ringWrite.store(write + 1, std::memory_order_release);
}

void UiHealthMonitor::writeStallReport(const Snapshot &snapshot,
                                       quint64 stallEpoch, quint64 lagMs)
{
    HANDLE log = CreateFileW(snapshot.logPath.data(), GENERIC_WRITE,
                             FILE_SHARE_READ, nullptr, CREATE_ALWAYS,
                             FILE_ATTRIBUTE_NORMAL, nullptr);
    char report[8192] = {};
    int used = std::snprintf(
        report, sizeof(report),
        "===== SpanTui UI stall =====\r\n"
        "stallEpoch=%llu lagMs=%llu pid=%lu guiTid=%lu watchdogTid=%lu\r\n"
        "transition.phase=%d transition.epoch=%llu visualCommit=%llu\r\n"
        "window.state=%d window.size=%dx%d\r\n"
        "terminal.sessions=%d "
        "input.bytes=%lld output.bytes=%lld\r\n"
        "monitor.queueDepth=%d monitor.pdhStatus=%d "
        "persist.queueDepth=%d\r\n"
        "gui.currentSystemCall=unavailable-without-suspending-gui\r\n"
        "dump=skipped-in-process\r\n",
        static_cast<unsigned long long>(stallEpoch), static_cast<unsigned long long>(lagMs),
        static_cast<unsigned long>(GetCurrentProcessId()),
        static_cast<unsigned long>(snapshot.guiThreadId.load(std::memory_order_relaxed)),
        static_cast<unsigned long>(GetCurrentThreadId()), snapshot.transitionPhase.load(std::memory_order_relaxed),
        static_cast<unsigned long long>(snapshot.transitionEpoch.load(std::memory_order_relaxed)),
        static_cast<unsigned long long>(snapshot.visualCommitRevision.load(std::memory_order_relaxed)),
        snapshot.windowState.load(std::memory_order_relaxed), snapshot.windowWidth.load(std::memory_order_relaxed),
        snapshot.windowHeight.load(std::memory_order_relaxed), snapshot.sessionCount.load(std::memory_order_relaxed),
        static_cast<long long>(snapshot.pendingInputBytes.load(std::memory_order_relaxed)),
        static_cast<long long>(snapshot.pendingOutputBytes.load(std::memory_order_relaxed)),
        snapshot.resourceSampleDepth.load(std::memory_order_relaxed),
        snapshot.resourcePdhStatus.load(std::memory_order_relaxed),
        snapshot.persistenceQueueDepth.load(std::memory_order_relaxed));
    if (used > 0 && static_cast<size_t>(used) < sizeof(report)) {
        const int appended = std::snprintf(
            report + used, sizeof(report) - static_cast<size_t>(used),
            "diagnosticRing:\r\n");
        if (appended > 0) {
            used += std::min<int>(appended,
                static_cast<int>(sizeof(report)
                    - static_cast<size_t>(used) - 1));
        }
    }
    const unsigned write = snapshot.ringWrite.load(std::memory_order_acquire);
    const unsigned count = std::min(write,
        static_cast<unsigned>(snapshot.ring.size()));
    for (unsigned offset = 0; offset < count && used > 0
         && static_cast<size_t>(used) < sizeof(report); ++offset) {
        const unsigned absolute = write - count + offset;
        const quint64 packed = snapshot.ring[absolute % snapshot.ring.size()].load(
            std::memory_order_acquire);
        const int appended = std::snprintf(
            report + used, sizeof(report) - static_cast<size_t>(used),
            "  t=%llu event=%u detail=%u\r\n",
            static_cast<unsigned long long>(packed >> 24),
            static_cast<unsigned>((packed >> 16) & 0xffu),
            static_cast<unsigned>(packed & 0xffffu));
        if (appended <= 0) {
            break;
        }
        used += std::min<int>(appended,
            static_cast<int>(sizeof(report) - static_cast<size_t>(used) - 1));
    }
    if (log != INVALID_HANDLE_VALUE) {
        writeAll(log, report, std::strlen(report));
        FlushFileBuffers(log);
        CloseHandle(log);
    }
}

void UiHealthMonitor::writeRecoveredRecord(const Snapshot &snapshot,
                                           quint64 stallEpoch, quint64 lagMs)
{
    HANDLE log = CreateFileW(snapshot.logPath.data(), FILE_APPEND_DATA,
                             FILE_SHARE_READ, nullptr, OPEN_ALWAYS,
                             FILE_ATTRIBUTE_NORMAL, nullptr);
    if (log == INVALID_HANDLE_VALUE) {
        return;
    }
    SetFilePointer(log, 0, nullptr, FILE_END);
    char record[256] = {};
    const int length = std::snprintf(
        record, sizeof(record),
        "recovered stallEpoch=%llu originalLagMs=%llu at=%llu\r\n",
        static_cast<unsigned long long>(stallEpoch),
        static_cast<unsigned long long>(lagMs),
        static_cast<unsigned long long>(monotonicMilliseconds()));
    if (length > 0) {
        writeAll(log, record,
                 static_cast<size_t>(std::min<int>(length,
                     static_cast<int>(sizeof(record) - 1))));
    }
    FlushFileBuffers(log);
    CloseHandle(log);
}

void UiHealthMonitor::watchdogLoop(const std::shared_ptr<Snapshot> &snapshot)
{
    if (!snapshot) {
        return;
    }
    bool stalled = false;
    quint64 stallEpoch = 0;
    quint64 stallSerial = 0;
    quint64 stallLag = 0;
    while (!snapshot->stopping.load(std::memory_order_acquire)) {
        {
            std::unique_lock lock(snapshot->wakeMutex);
            if (snapshot->wake.wait_for(lock, std::chrono::milliseconds(kWatchdogPollMs),
                                        [&snapshot]() { return snapshot->stopping.load(std::memory_order_acquire); })) {
                break;
            }
        }
        const quint64 now = monotonicMilliseconds();
        const quint64 heartbeat = snapshot->lastHeartbeatMs.load(
            std::memory_order_acquire);
        const quint64 serial = snapshot->heartbeatSerial.load(
            std::memory_order_acquire);
        const quint64 lag = now >= heartbeat ? now - heartbeat : 0;
        const quint64 threshold = static_cast<quint64>(snapshot->stallThresholdMs)
            + static_cast<quint64>(snapshot->heartbeatSlackMs.load(std::memory_order_acquire));
        if (!stalled && heartbeat != 0 && lag >= threshold) {
            stalled = true;
            stallSerial = serial;
            stallLag = lag;
            writeStallReport(*snapshot, ++stallEpoch, lag);
        } else if (stalled && serial != stallSerial) {
            writeRecoveredRecord(*snapshot, stallEpoch, stallLag);
            stalled = false;
        }
    }
}

UiHealthMonitor::UiHealthMonitor(span::application::WindowTransitionCoordinator *transition,
                                 span::terminal::TerminalManager *terminals, ResourceMonitor *resources,
                                 span::application::SettingsStore *settings, QQuickWindow *window,
                                 const QString &diagnosticDirectory, int stallThresholdMs, QObject *parent)
    : QObject(parent),
      m_transition(transition),
      m_terminals(terminals),
      m_resources(resources),
      m_settings(settings),
      m_window(window),
      m_snapshot(std::make_shared<Snapshot>())
{
    // Below one heartbeat every normal gap would read as a stall.
    m_snapshot->stallThresholdMs = std::max(kHeartbeatMs, stallThresholdMs);
    const QString directory = diagnosticDirectory.isEmpty()
        ? span::core::ProductPaths::hangDiagnosticDirectory()
        : diagnosticDirectory;
    QDir().mkpath(directory);
    storePath(QDir(directory).filePath(QStringLiteral("hang-latest.log")),
              &m_snapshot->logPath);
    m_heartbeatTimer.setInterval(kHeartbeatMs);
    m_heartbeatTimer.setTimerType(Qt::PreciseTimer);
    connect(&m_heartbeatTimer, &QTimer::timeout,
            this, &UiHealthMonitor::pulse);
    if (m_transition) {
        connect(m_transition, &span::application::WindowTransitionCoordinator::phaseChanged, this,
                &UiHealthMonitor::applyHeartbeatInterval);
        applyHeartbeatInterval();
    }
}

void UiHealthMonitor::applyHeartbeatInterval()
{
    const bool minimized =
        m_transition && m_transition->phase() == span::application::WindowTransitionCoordinator::Minimized;
    const int interval = minimized ? kHeartbeatMs * kMinimizedBackoff : kHeartbeatMs;
    if (m_heartbeatTimer.interval() == interval) {
        return;
    }
    if (minimized) {
        // Widen the threshold before the pulse slows down.
        m_snapshot->heartbeatSlackMs.store(interval - kHeartbeatMs, std::memory_order_release);
    } else if (m_started.load(std::memory_order_acquire)) {
        // Narrow it only after a fresh pulse.
        pulse();
    }
    m_heartbeatTimer.setInterval(interval);
    if (!minimized) {
        m_snapshot->heartbeatSlackMs.store(0, std::memory_order_release);
    }
}

UiHealthMonitor::~UiHealthMonitor()
{
    m_heartbeatTimer.stop();
    const auto snapshot = m_snapshot;
    if (!snapshot) {
        return;
    }
    snapshot->stopping.store(true, std::memory_order_release);
    snapshot->wake.notify_one();
    if (m_watchdog.joinable()) {
        // Never join a watchdog that may be inside a stall report. The
        // thread owns Snapshot via shared_ptr, so GUI teardown cannot UAF.
        m_watchdog.detach();
    }
}

void UiHealthMonitor::start()
{
    bool expected = false;
    if (!m_started.compare_exchange_strong(expected, true)) {
        return;
    }
    pulse();
    m_heartbeatTimer.start();
    const auto snapshot = m_snapshot;
    m_watchdog = std::thread([snapshot]() {
        UiHealthMonitor::watchdogLoop(snapshot);
    });

}

void UiHealthMonitor::pulse()
{
    Snapshot *snapshot = m_snapshot.get();
    if (!snapshot) {
        return;
    }
    snapshot->guiThreadId.store(GetCurrentThreadId(),
                                std::memory_order_relaxed);
    if (m_transition) {
        snapshot->transitionPhase.store(static_cast<int>(m_transition->phase()),
                                        std::memory_order_relaxed);
        snapshot->transitionEpoch.store(m_transition->epoch(),
                                        std::memory_order_relaxed);
        snapshot->visualCommitRevision.store(m_transition->commitRevision(),
                                             std::memory_order_relaxed);
    }
    if (m_window) {
        snapshot->windowState.store(static_cast<int>(m_window->windowStates()),
                                    std::memory_order_relaxed);
        snapshot->windowWidth.store(m_window->width(),
                                    std::memory_order_relaxed);
        snapshot->windowHeight.store(m_window->height(),
                                     std::memory_order_relaxed);
    }
    if (m_terminals) {
        int sessionCount = 0;
        qint64 inputBytes = 0;
        qint64 outputBytes = 0;
        m_terminals->forEachSession([&](span::terminal::TerminalSession *session) {
            if (!session) {
                return;
            }
            ++sessionCount;
            inputBytes += session->pendingInputBytes();
            outputBytes += session->pendingOutputBytes();
        });
        snapshot->sessionCount.store(sessionCount, std::memory_order_relaxed);
        snapshot->pendingInputBytes.store(inputBytes,
                                          std::memory_order_relaxed);
        snapshot->pendingOutputBytes.store(outputBytes,
                                           std::memory_order_relaxed);
    }

    snapshot->resourceSampleDepth.store(
        m_resources && m_resources->samplingInProgress() ? 1 : 0,
        std::memory_order_relaxed);
    snapshot->resourcePdhStatus.store(
        m_resources ? m_resources->pdhStatus() : 0,
        std::memory_order_relaxed);
    snapshot->persistenceQueueDepth.store(m_settings ? m_settings->persistenceQueueDepth() : 0,
                                          std::memory_order_relaxed);
    recordRing(snapshot, 1, static_cast<quint16>(snapshot->transitionPhase.load(
                          std::memory_order_relaxed)));
    snapshot->lastHeartbeatMs.store(monotonicMilliseconds(),
                                    std::memory_order_release);
    snapshot->heartbeatSerial.fetch_add(1, std::memory_order_release);
}

} // namespace span::application::diagnostics
