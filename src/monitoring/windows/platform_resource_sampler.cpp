#include "monitoring/platform_resource_sampler.h"

#include "monitoring/gpu_engine_metrics.h"

#include <QSet>
#include <QVector>

#include <windows.h>
#include <psapi.h>
#include <tlhelp32.h>
#include <pdh.h>
#include <pdhmsg.h>

namespace {

qint64 fileTimesToMilliseconds(const FILETIME &kernel, const FILETIME &user)
{
    ULARGE_INTEGER kernelTime{};
    kernelTime.LowPart = kernel.dwLowDateTime;
    kernelTime.HighPart = kernel.dwHighDateTime;
    ULARGE_INTEGER userTime{};
    userTime.LowPart = user.dwLowDateTime;
    userTime.HighPart = user.dwHighDateTime;
    return qint64((kernelTime.QuadPart + userTime.QuadPart) / 10000);
}

qint64 processCpuMilliseconds()
{
    FILETIME created{}, exited{}, kernel{}, user{};
    if (!GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user)) {
        return 0;
    }
    return fileTimesToMilliseconds(kernel, user);
}

int systemProcessorCount()
{
    SYSTEM_INFO info{};
    GetSystemInfo(&info);
    return qMax(1, int(info.dwNumberOfProcessors));
}

struct ProcessUsage {
    qint64 cpuMs = 0;
    qint64 privateBytes = 0;
    qint64 workingSetBytes = 0;
};

bool sampleProcess(DWORD processId, ProcessUsage *out)
{
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
    const bool gotMem =
        GetProcessMemoryInfo(handle, reinterpret_cast<PROCESS_MEMORY_COUNTERS *>(&counters), sizeof(counters));
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
    for (BOOL found = Process32FirstW(snapshot, &entry); found; found = Process32NextW(snapshot, &entry)) {
        children[entry.th32ParentProcessID].append(entry.th32ProcessID);
    }
    CloseHandle(snapshot);
    return children;
}

QSet<DWORD> collectProcessTree(DWORD rootProcessId, const QHash<DWORD, QVector<DWORD>> &children)
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

void ensureGpuQuery(void **query, void **counter)
{
    if (!query || !counter || *query) {
        return;
    }
    PDH_HQUERY opened = nullptr;
    if (PdhOpenQueryW(nullptr, 0, &opened) != ERROR_SUCCESS) {
        return;
    }
    PDH_HCOUNTER added = nullptr;
    if (PdhAddEnglishCounterW(opened, L"\\GPU Engine(*)\\Utilization Percentage", 0, &added) != ERROR_SUCCESS) {
        PdhCloseQuery(opened);
        return;
    }
    PdhCollectQueryData(opened);
    *query = opened;
    *counter = added;
}

QVector<GpuEngineReading> collectGpuReadings(void *query, void *counter, bool *succeeded)
{
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
    PDH_STATUS status =
        PdhGetFormattedCounterArrayW(static_cast<PDH_HCOUNTER>(counter), PDH_FMT_DOUBLE, &size, &count, nullptr);
    if (status != PDH_MORE_DATA && status != ERROR_SUCCESS) {
        return out;
    }
    QByteArray buffer(int(size), 0);
    auto *items = reinterpret_cast<PDH_FMT_COUNTERVALUE_ITEM_W *>(buffer.data());
    status = PdhGetFormattedCounterArrayW(static_cast<PDH_HCOUNTER>(counter), PDH_FMT_DOUBLE, &size, &count, items);
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

    Result sample(const QVector<SessionTarget> &targets, const QHash<QString, SessionSample> &previousSessions,
                  qint64 elapsedMs) override
    {
        Result result;
        const int processors = systemProcessorCount();
        const qint64 cpuMs = processCpuMilliseconds();
        if (elapsedMs > 0 && m_lastProcessCpuMs > 0) {
            result.cpuPercent =
                qBound(0.0, 100.0 * double(cpuMs - m_lastProcessCpuMs) / double(elapsedMs) / double(processors), 100.0);
        }

        m_lastProcessCpuMs = cpuMs;

        ensureGpuQuery(&m_gpuPdhQuery, &m_gpuPdhCounter);
        bool gpuQuerySucceeded = false;
        const QVector<GpuEngineReading> gpuReadings =
            collectGpuReadings(m_gpuPdhQuery, m_gpuPdhCounter, &gpuQuerySucceeded);
        result.gpuCounterStatus = gpuQuerySucceeded
            ? GpuCounterStatus::Ready
            : (m_gpuPdhQuery && m_gpuPdhCounter ? GpuCounterStatus::QueryFailed : GpuCounterStatus::NotInitialized);
        result.gpuPercent = gpuPercentForPids(gpuReadings, {quint32(GetCurrentProcessId())});

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
                const auto previous = session.lastCpuByPid.constFind(quint32(processId));
                if (previous != session.lastCpuByPid.cend() && one.cpuMs >= previous.value()) {
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
                ? qBound(0.0, 100.0 * double(cpuDeltaMs) / double(elapsedMs) / double(processors), 100.0)
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

} // namespace

std::shared_ptr<ResourceSampleProvider> createPlatformResourceSampleProvider()
{
    return std::make_shared<PlatformResourceSampleProvider>();
}

qint64 installedPhysicalMemoryBytes()
{
    MEMORYSTATUSEX memory{};
    memory.dwLength = sizeof(memory);
    return GlobalMemoryStatusEx(&memory) ? qint64(memory.ullTotalPhys) : 0;
}

OwnProcessCounters ownProcessCounters()
{
    OwnProcessCounters result;
    PROCESS_MEMORY_COUNTERS_EX counters{};
    if (GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS *>(&counters),
                             sizeof(counters))) {
        result.privateBytes = qint64(counters.PrivateUsage);
        result.workingSetBytes = qint64(counters.WorkingSetSize);
    }
    DWORD handles = 0;
    if (GetProcessHandleCount(GetCurrentProcess(), &handles)) {
        result.handles = int(handles);
    }
    HANDLE threadSnapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (threadSnapshot != INVALID_HANDLE_VALUE) {
        int threadCount = 0;
        THREADENTRY32 entry{};
        entry.dwSize = sizeof(entry);
        for (BOOL found = Thread32First(threadSnapshot, &entry); found; found = Thread32Next(threadSnapshot, &entry)) {
            if (entry.th32OwnerProcessID == GetCurrentProcessId()) {
                ++threadCount;
            }
        }
        CloseHandle(threadSnapshot);
        result.threads = threadCount;
    }
    return result;
}
