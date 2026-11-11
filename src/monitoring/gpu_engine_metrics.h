#pragma once

#include <QSet>
#include <QStringView>
#include <QVector>

// WDDM GPU Engine PDH instance names look like
// `pid_1234_luid_0x00000000_0x00013BF4_phys_0_eng_0_engtype_3D`.
struct GpuEngineReading {
    quint32 pid = 0;
    double utilization = 0;
};

quint32 parseGpuEnginePid(QStringView instanceName);

// Peak engine utilization among the given PIDs. Matches Task Manager's
// per-process GPU column: engines run in parallel, so values are not summed.
double gpuPercentForPids(const QVector<GpuEngineReading> &readings, const QSet<quint32> &pids);
