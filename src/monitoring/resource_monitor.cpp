#include "monitoring/resource_monitor.h"

#include "monitoring/gpu_engine_metrics.h"

#include <QByteArray>
#include <QSet>
#include <QTimer>
#include <QVector>

#include <condition_variable>
#include <mutex>
#include <thread>
#include <utility>

#include <windows.h>
#include <psapi.h>
#include <tlhelp32.h>
#include <pdh.h>
#include <pdhmsg.h>

namespace {
// Polls the sampling thread's result.
constexpr int kCompletionPollMs = 50;

qint64 fileTimesToMilliseconds(const FILETIME &kernel, const FILETIME &user) {
    ULARGE_INTEGER kernelTime{};
    kernelTime.LowPart = kernel.dwLowDateTime;
    kernelTime.HighPart = kernel.dwHighDateTime;
    ULARGE_INTEGER userTime{};
    userTime.LowPart = user.dwLowDateTime;
    userTime.HighPart = user.dwHighDateTime;
    return qint64((kernelTime.QuadPart + userTime.QuadPart) / 10000);
}

qint64 processCpuMilliseconds() {
    FILETIME created{}, exited{}, kernel{}, user{};
    if (!GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user)) {
        return 0;
    }
    return fileTimesToMilliseconds(kernel, user);
}

int systemProcessorCount() {
    SYSTEM_INFO info{};
    GetSystemInfo(&info);
    return qMax(1, int(info.dwNumberOfProcessors));
}

struct ProcessUsage {
    qint64 cpuMs = 0;
    qint64 privateBytes = 0;
    qint64 workingSetBytes = 0;
};

bool sampleProcess(DWORD processId, ProcessUsage *out) {
    if (!out || processId == 0) {
        return false;
    }
    const HANDLE handle = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, processId);
    if (!handle) {
        return false;
    }

    FILETIME created{}, exited{}, kernel{}, user{};
    PROCESS_MEMORY_COUNTERS_EX counters{};
    const bool gotTimes = GetProcessTimes(handle, &created, &exited, &kernel, &user);
    const bool gotMem = GetProcessMemoryInfo(handle,
                                             reinterpret_cast<PROCESS_MEMORY_COUNTERS *>(&counters),
                                             sizeof(counters));
    CloseHandle(handle);
    if (!gotTimes && !gotMem) {
        return false;
    }
    if (gotTimes) {
        out->cpuMs = fileTimesToMilliseconds(kernel, user);
    }
    if (gotMem) {
        // PrivateUsage ≈ Task Manager "Commit size" / private bytes.
        // WorkingSetSize ≈ Task Manager default "Memory" column.
        out->privateBytes = qint64(counters.PrivateUsage);
        out->workingSetBytes = qint64(counters.WorkingSetSize);
    }
    return true;
}

QHash<DWORD, QVector<DWORD>> snapshotProcessChildren()
{
    QHash<DWORD, QVector<DWORD>> children;
    const HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) {
        return children;
    }

    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    for (BOOL found = Process32FirstW(snapshot, &entry); found;
         found = Process32NextW(snapshot, &entry)) {
        children[entry.th32ParentProcessID].append(entry.th32ProcessID);
    }
    CloseHandle(snapshot);
    return children;
}

QSet<DWORD> collectProcessTree(
    DWORD rootProcessId, const QHash<DWORD, QVector<DWORD>> &children)
{
    QSet<DWORD> tree;
    if (rootProcessId == 0) {
        return tree;
    }

    QVector<DWORD> stack{rootProcessId};
    while (!stack.isEmpty()) {
        const DWORD id = stack.takeLast();
        if (tree.contains(id)) {
            continue;
        }
        tree.insert(id);
        const auto kids = children.constFind(id);
        if (kids == children.cend()) {
            continue;
        }
        for (DWORD child : *kids) {
            stack.append(child);
        }
    }
    return tree;
}


void ensureGpuQuery(void **query, void **counter) {
    if (!query || !counter || *query) {
        return;
    }
    PDH_HQUERY opened = nullptr;
    if (PdhOpenQueryW(nullptr, 0, &opened) != ERROR_SUCCESS) {
        return;
    }
    PDH_HCOUNTER added = nullptr;
    if (PdhAddEnglishCounterW(opened,
                              L"\\GPU Engine(*)\\Utilization Percentage",
                              0,
                              &added) != ERROR_SUCCESS) {
        PdhCloseQuery(opened);
        return;
    }
    PdhCollectQueryData(opened);
    *query = opened;
    *counter = added;
}

QVector<GpuEngineReading> collectGpuReadings(void *query, void *counter,
                                             bool *succeeded) {
    QVector<GpuEngineReading> out;
    if (succeeded) {
        *succeeded = false;
    }
    if (!query || !counter) {
        return out;
    }
    if (PdhCollectQueryData(static_cast<PDH_HQUERY>(query)) != ERROR_SUCCESS) {
        return out;
    }
    DWORD size = 0;
    DWORD count = 0;
    PDH_STATUS status = PdhGetFormattedCounterArrayW(
        static_cast<PDH_HCOUNTER>(counter), PDH_FMT_DOUBLE, &size, &count, nullptr);
    if (status != PDH_MORE_DATA && status != ERROR_SUCCESS) {
        return out;
    }
    QByteArray buffer(int(size), 0);
    auto *items = reinterpret_cast<PDH_FMT_COUNTERVALUE_ITEM_W *>(buffer.data());
    status = PdhGetFormattedCounterArrayW(
        static_cast<PDH_HCOUNTER>(counter), PDH_FMT_DOUBLE, &size, &count, items);
    if (status != ERROR_SUCCESS) {
        return out;
    }
    if (succeeded) {
        *succeeded = true;
    }
    out.reserve(int(count));
    for (DWORD i = 0; i < count; ++i) {
        if (items[i].FmtValue.CStatus != ERROR_SUCCESS) {
            continue;
        }
        GpuEngineReading reading;
        reading.pid = parseGpuEnginePid(QString::fromWCharArray(items[i].szName));
        reading.utilization = items[i].FmtValue.doubleValue;
        if (reading.pid != 0) {
            out.push_back(reading);
        }
    }
    return out;
}

class PlatformResourceSampleProvider final : public ResourceSampleProvider {
public:
    ~PlatformResourceSampleProvider() override
    {
        if (m_gpuPdhQuery) {
            PdhCloseQuery(static_cast<PDH_HQUERY>(m_gpuPdhQuery));
        }
    }

    Result sample(const QVector<SessionTarget> &targets,
                  const QHash<QString, SessionSample> &previousSessions,
                  qint64 elapsedMs) override
    {
        Result result;
        const int processors = systemProcessorCount();
        const qint64 cpuMs = processCpuMilliseconds();
        if (elapsedMs > 0 && m_lastProcessCpuMs > 0) {
            result.cpuPercent = qBound(
                0.0,
                100.0 * double(cpuMs - m_lastProcessCpuMs)
                    / double(elapsedMs) / double(processors),
                100.0);
        }

        m_lastProcessCpuMs = cpuMs;

        ensureGpuQuery(&m_gpuPdhQuery, &m_gpuPdhCounter);
        bool gpuQuerySucceeded = false;
        const QVector<GpuEngineReading> gpuReadings =
            collectGpuReadings(m_gpuPdhQuery, m_gpuPdhCounter,
                               &gpuQuerySucceeded);
        result.pdhStatus = gpuQuerySucceeded
            ? PdhStatus::Ready
            : (m_gpuPdhQuery && m_gpuPdhCounter
                   ? PdhStatus::QueryFailed
                   : PdhStatus::NotInitialized);
        result.gpuPercent = gpuPercentForPids(
            gpuReadings, {quint32(GetCurrentProcessId())});

        const QHash<DWORD, QVector<DWORD>> children = snapshotProcessChildren();
        for (const SessionTarget &target : targets) {
            SessionSample session = previousSessions.value(target.id);
            if (target.rootPid == 0 || !target.running) {
                result.sessions.insert(target.id, SessionSample{});
                continue;
            }
            QSet<DWORD> tree = collectProcessTree(DWORD(target.rootPid), children);
            for (quint32 processId : target.jobPids) {
                if (processId != 0) {
                    tree.insert(DWORD(processId));
                }
            }

            QHash<quint32, qint64> nextCpuByPid;
            qint64 totalPrivate = 0;
            qint64 totalWorking = 0;
            qint64 cpuDeltaMs = 0;
            bool any = false;
            for (DWORD processId : tree) {
                ProcessUsage one;
                if (!sampleProcess(processId, &one)) {
                    continue;
                }
                any = true;
                totalPrivate += one.privateBytes;
                totalWorking += one.workingSetBytes;
                nextCpuByPid.insert(quint32(processId), one.cpuMs);
                const auto previous = session.lastCpuByPid.constFind(
                    quint32(processId));
                if (previous != session.lastCpuByPid.cend()
                    && one.cpuMs >= previous.value()) {
                    cpuDeltaMs += one.cpuMs - previous.value();
                }
            }
            session.privateBytes = totalPrivate;
            session.workingSetBytes = totalWorking;
            session.lastCpuByPid = nextCpuByPid;
            QSet<quint32> gpuPids;
            for (DWORD processId : tree) {
                gpuPids.insert(quint32(processId));
            }
            session.gpuPercent = gpuPercentForPids(gpuReadings, gpuPids);
            session.cpuPercent = elapsedMs > 0 && any
                ? qBound(0.0,
                         100.0 * double(cpuDeltaMs) / double(elapsedMs)
                             / double(processors),
                         100.0)
                : 0.0;

            session.valid = any;
            result.sessions.insert(target.id, session);
        }
        return result;
    }

private:
    qint64 m_lastProcessCpuMs = 0;
    void *m_gpuPdhQuery = nullptr;
    void *m_gpuPdhCounter = nullptr;
};
}

struct ResourceMonitor::CallbackState {
    std::shared_ptr<const ResourceSampleProvider::Result> completed;
};

struct ResourceMonitor::SampleLane {
    std::mutex mutex;
    std::condition_variable changed;
    std::shared_ptr<ResourceSampleProvider> provider;
    std::shared_ptr<CallbackState> callbackState;
    QVector<ResourceSampleProvider::SessionTarget> targets;
    QHash<QString, ResourceSampleProvider::SessionSample> previousSessions;
    qint64 elapsedMs = 0;
    bool hasRequest = false;
    bool stop = false;
    std::thread worker;

    ~SampleLane()
    {
        if (worker.joinable()) {
            worker.detach();
        }
    }
};

ResourceMonitor::ResourceMonitor(SessionTargetSource targetSource, QObject *parent,
                                 std::shared_ptr<ResourceSampleProvider> sampleProvider)
    : QObject(parent),
      m_targetSource(std::move(targetSource)),
      m_timer(new QTimer(this)),
      m_completionTimer(new QTimer(this)),
      m_sampleProvider(sampleProvider ? std::move(sampleProvider) : std::make_shared<PlatformResourceSampleProvider>()),
      m_callbackState(std::make_shared<CallbackState>()),
      m_sampleLane(std::make_shared<SampleLane>())
{
    MEMORYSTATUSEX memory{};
    memory.dwLength = sizeof(memory);
    if (GlobalMemoryStatusEx(&memory)) {
        m_physicalMemoryBytes = qint64(memory.ullTotalPhys);
    }
    m_sampleLane->provider = m_sampleProvider;
    m_sampleLane->callbackState = m_callbackState;
    const auto lane = m_sampleLane;
    m_sampleLane->worker = std::thread([lane]() {
        while (true) {
            std::unique_lock<std::mutex> lock(lane->mutex);
            lane->changed.wait(lock, [lane]() {
                return lane->stop || lane->hasRequest;
            });
            if (lane->stop) {
                lane->hasRequest = false;
                break;
            }
            auto provider = lane->provider;
            auto callbackState = lane->callbackState;
            auto targets = std::move(lane->targets);
            auto previousSessions = std::move(lane->previousSessions);
            const qint64 elapsedMs = lane->elapsedMs;
            lane->hasRequest = false;
            lock.unlock();

            ResourceSampleProvider::Result sampled;
            try {
                sampled = provider->sample(
                    targets, previousSessions, elapsedMs);
            } catch (...) {
                sampled.pdhStatus =
                    ResourceSampleProvider::PdhStatus::ProviderFailed;
            }
            const auto result = std::make_shared<
                const ResourceSampleProvider::Result>(std::move(sampled));
            std::atomic_store_explicit(
                &callbackState->completed, result, std::memory_order_release);
        }
        lane->provider.reset();
    });
    m_timer->setTimerType(Qt::CoarseTimer);
    connect(m_timer, &QTimer::timeout, this, &ResourceMonitor::refresh);
    m_completionTimer->setInterval(kCompletionPollMs);
    m_completionTimer->setTimerType(Qt::CoarseTimer);
    connect(m_completionTimer, &QTimer::timeout,
            this, &ResourceMonitor::pollSampleCompletion);
    m_elapsed.start();
    // Inactive until a visible consumer asks for metrics.
    applyEffectiveInterval();
}

ResourceMonitor::~ResourceMonitor() {
    m_timer->stop();
    m_completionTimer->stop();
    if (!m_sampleLane) {
        return;
    }
    {
        std::lock_guard<std::mutex> lock(m_sampleLane->mutex);
        m_sampleLane->stop = true;
    }
    m_sampleLane->changed.notify_one();
    if (m_sampleLane->worker.joinable()) {
        m_sampleLane->worker.detach();
    }
    m_sampleLane.reset();
}

double ResourceMonitor::cpuPercent() const { return m_cpuPercent; }
double ResourceMonitor::gpuPercent() const { return m_gpuPercent; }

int ResourceMonitor::effectiveIntervalMs() const {
    return m_intervalSeconds * 1000 * (m_idle ? 5 : 1);
}

void ResourceMonitor::applyEffectiveInterval() {
    m_timer->setInterval(effectiveIntervalMs());
    if (m_active) m_timer->start();
}

void ResourceMonitor::setIntervalSeconds(int seconds) {
    seconds = qBound(1, seconds, 60);
    if (m_intervalSeconds == seconds) {
        return;
    }
    m_intervalSeconds = seconds;
    applyEffectiveInterval();
}

void ResourceMonitor::setActive(bool active)
{
    if (m_active == active) {
        return;
    }
    m_active = active;
    if (!m_active) {
        m_timer->stop();
        return;
    }
    refresh();
    applyEffectiveInterval();
}

void ResourceMonitor::setConsumerVisible(QObject *consumer, bool visible)
{
    if (!consumer) {
        return;
    }
    if (visible && !m_visibleConsumers.contains(consumer)) {
        m_visibleConsumers.insert(consumer, connect(consumer, &QObject::destroyed, this, [this](QObject *gone) {
                                      m_visibleConsumers.remove(gone);
                                      setActive(!m_visibleConsumers.isEmpty());
                                  }));
    } else if (!visible) {
        disconnect(m_visibleConsumers.take(consumer));
    }
    setActive(!m_visibleConsumers.isEmpty());
}

void ResourceMonitor::setIdle(bool idle) {
    if (m_idle == idle) {
        return;
    }
    m_idle = idle;
    emit idleChanged();
    if (!m_idle && m_active) {
        // Waking resamples at once (still bounded by the single-in-flight
        // guard) so the meter does not show a stale idle-period value.
        refresh();
    }
    applyEffectiveInterval();
}

QVariantMap ResourceMonitor::sessionMetrics(const QString &sessionId) const {
    QVariantMap metrics;
    // Every target of a sampling round has an entry (stopped sessions an
    // invalid one), so an unknown id yields no metrics.
    const auto found = m_sessionSamples.constFind(sessionId);
    if (found == m_sessionSamples.constEnd()) {
        return metrics;
    }
    const ResourceSampleProvider::SessionSample &sample = *found;
    metrics.insert(QStringLiteral("cpuPercent"), sample.valid ? sample.cpuPercent : 0.0);
    metrics.insert(QStringLiteral("privateBytes"), sample.valid ? sample.privateBytes : qint64(0));
    metrics.insert(QStringLiteral("workingSetBytes"), sample.valid ? sample.workingSetBytes : qint64(0));
    // Prefer working set for UI "memory" — matches Task Manager's default column.
    metrics.insert(QStringLiteral("gpuPercent"), sample.valid ? sample.gpuPercent : 0.0);
    metrics.insert(QStringLiteral("memoryBytes"),
                   sample.valid ? sample.workingSetBytes : qint64(0));
    return metrics;
}

void ResourceMonitor::refresh() {
    if (m_samplingInProgress || !m_targetSource || !m_sampleProvider) {
        return;
    }
    const qint64 elapsedMs = m_elapsed.restart();
    m_samplingInProgress = true;
    QVector<ResourceSampleProvider::SessionTarget> targets = m_targetSource();
    const auto previousSamples = m_sessionSamples;
    m_completionTimer->start();
    {
        std::lock_guard<std::mutex> lock(m_sampleLane->mutex);
        if (m_sampleLane->stop) {
            m_samplingInProgress = false;
            m_completionTimer->stop();
            return;
        }
        m_sampleLane->targets = std::move(targets);
        m_sampleLane->previousSessions = previousSamples;
        m_sampleLane->elapsedMs = elapsedMs;
        m_sampleLane->hasRequest = true;
    }
    m_sampleLane->changed.notify_one();
}

void ResourceMonitor::pollSampleCompletion()
{
    const auto result = std::atomic_exchange_explicit(
        &m_callbackState->completed,
        std::shared_ptr<const ResourceSampleProvider::Result>{},
        std::memory_order_acq_rel);
    if (!result) {
        return;
    }
    m_completionTimer->stop();
    applySample(*result);
}

void ResourceMonitor::applySample(const ResourceSampleProvider::Result &sample)
{
    m_sessionSamples = sample.sessions;
    m_pdhStatus = sample.pdhStatus;
    m_cpuPercent = sample.cpuPercent;
    m_gpuPercent = sample.gpuPercent;
    m_samplingInProgress = false;
    emit metricsChanged();
}
