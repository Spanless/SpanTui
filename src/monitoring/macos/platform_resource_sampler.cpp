#include "monitoring/platform_resource_sampler.h"

#include <QSet>
#include <QVector>

#include <vector>

#include <libproc.h>
#include <mach/mach.h>
#include <mach/mach_time.h>
#include <sys/sysctl.h>
#include <unistd.h>

namespace {

// Children listed per process; a shell's foreground job rarely has more.
constexpr int kMaxChildrenPerProcess = 256;

struct ProcessUsage {
    qint64 cpuMs = 0;
    qint64 residentBytes = 0;
};

// proc_taskinfo reports CPU time in Mach absolute-time units: nanoseconds on
// Intel, 1/24 µs ticks on Apple silicon.
qint64 machTicksToMilliseconds(quint64 ticks)
{
    static const mach_timebase_info_data_t timebase = [] {
        mach_timebase_info_data_t info{};
        mach_timebase_info(&info);
        return info;
    }();
    if (timebase.denom == 0) return 0;
    return qint64(ticks * timebase.numer / timebase.denom / 1000000ULL);
}

bool sampleProcess(pid_t pid, ProcessUsage *out)
{
    proc_taskinfo info{};
    if (pid <= 0 || proc_pidinfo(pid, PROC_PIDTASKINFO, 0, &info, sizeof(info)) != int(sizeof(info))) {
        return false;
    }
    out->cpuMs = machTicksToMilliseconds(info.pti_total_user + info.pti_total_system);
    out->residentBytes = qint64(info.pti_resident_size);
    return true;
}

QSet<quint32> collectProcessTree(pid_t root)
{
    QSet<quint32> tree;
    QVector<pid_t> pending{root};
    pid_t children[kMaxChildrenPerProcess];
    while (!pending.isEmpty()) {
        const pid_t pid = pending.takeLast();
        if (tree.contains(quint32(pid))) continue;
        tree.insert(quint32(pid));
        // A pid count (-1 on failure), at most what the buffer holds.
        const int count = qMin(proc_listchildpids(pid, children, int(sizeof(children))), kMaxChildrenPerProcess);
        for (int i = 0; i < count; ++i) {
            if (children[i] > 0) pending.append(children[i]);
        }
    }
    return tree;
}

int processorCount()
{
    const long count = sysconf(_SC_NPROCESSORS_ONLN);
    return count > 0 ? int(count) : 1;
}

class PlatformResourceSampleProvider final : public ResourceSampleProvider {
public:
    Result sample(const QVector<SessionTarget> &targets, const QHash<QString, SessionSample> &previousSessions,
                  qint64 elapsedMs) override
    {
        Result result;
        const int processors = processorCount();
        ProcessUsage self;
        if (sampleProcess(getpid(), &self)) {
            if (elapsedMs > 0 && m_lastProcessCpuMs > 0) {
                result.cpuPercent = qBound(
                    0.0, 100.0 * double(self.cpuMs - m_lastProcessCpuMs) / double(elapsedMs) / double(processors),
                    100.0);
            }
            m_lastProcessCpuMs = self.cpuMs;
        }
        result.gpuCounterStatus = GpuCounterStatus::Unavailable;

        for (const SessionTarget &target : targets) {
            SessionSample session = previousSessions.value(target.id);
            if (target.rootPid == 0 || !target.running) {
                result.sessions.insert(target.id, SessionSample{});
                continue;
            }
            QSet<quint32> tree = collectProcessTree(pid_t(target.rootPid));
            for (quint32 processId : target.jobPids) {
                if (processId != 0) tree.insert(processId);
            }
            QHash<quint32, qint64> nextCpuByPid;
            qint64 totalResident = 0;
            qint64 cpuDeltaMs = 0;
            bool any = false;
            for (quint32 processId : tree) {
                ProcessUsage one;
                if (!sampleProcess(pid_t(processId), &one)) continue;
                any = true;
                totalResident += one.residentBytes;
                nextCpuByPid.insert(processId, one.cpuMs);
                const auto previous = session.lastCpuByPid.constFind(processId);
                if (previous != session.lastCpuByPid.cend() && one.cpuMs >= previous.value()) {
                    cpuDeltaMs += one.cpuMs - previous.value();
                }
            }
            // macOS has no private-bytes counter per process; resident memory
            // is the closest figure for both values.
            session.privateBytes = totalResident;
            session.workingSetBytes = totalResident;
            session.lastCpuByPid = nextCpuByPid;
            session.gpuPercent = 0;
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
};

} // namespace

std::shared_ptr<ResourceSampleProvider> createPlatformResourceSampleProvider()
{
    return std::make_shared<PlatformResourceSampleProvider>();
}

qint64 installedPhysicalMemoryBytes()
{
    quint64 bytes = 0;
    size_t size = sizeof(bytes);
    return sysctlbyname("hw.memsize", &bytes, &size, nullptr, 0) == 0 ? qint64(bytes) : 0;
}

OwnProcessCounters ownProcessCounters()
{
    OwnProcessCounters result;
    const pid_t self = getpid();
    proc_taskinfo task{};
    if (proc_pidinfo(self, PROC_PIDTASKINFO, 0, &task, sizeof(task)) == int(sizeof(task))) {
        result.workingSetBytes = qint64(task.pti_resident_size);
        result.threads = int(task.pti_threadnum);
    }
    task_vm_info_data_t vm{};
    mach_msg_type_number_t count = TASK_VM_INFO_COUNT;
    if (task_info(mach_task_self(), TASK_VM_INFO, reinterpret_cast<task_info_t>(&vm), &count) == KERN_SUCCESS) {
        result.privateBytes = qint64(vm.phys_footprint);
    }
    const int bytes = proc_pidinfo(self, PROC_PIDLISTFDS, 0, nullptr, 0);
    if (bytes > 0) {
        std::vector<proc_fdinfo> descriptors(size_t(bytes) / sizeof(proc_fdinfo));
        const int filled =
            proc_pidinfo(self, PROC_PIDLISTFDS, 0, descriptors.data(), int(descriptors.size() * sizeof(proc_fdinfo)));
        if (filled >= 0) result.handles = filled / int(sizeof(proc_fdinfo));
    }
    return result;
}
