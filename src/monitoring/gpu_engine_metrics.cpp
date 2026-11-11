#include "monitoring/gpu_engine_metrics.h"

#include <QString>
#include <limits>

quint32 parseGpuEnginePid(QStringView instanceName)
{
    const QString text = instanceName.toString();
    const qsizetype at = text.indexOf(QLatin1String("pid_"), 0, Qt::CaseInsensitive);
    if (at < 0) {
        return 0;
    }

    qsizetype i = at + 4;
    quint64 pid = 0;
    bool any = false;
    while (i < text.size()) {
        const ushort ch = text.at(i).unicode();
        if (ch < '0' || ch > '9') {
            break;
        }
        any = true;
        pid = pid * 10ull + quint64(ch - '0');
        if (pid > std::numeric_limits<quint32>::max()) {
            return 0;
        }
        ++i;
    }
    return any ? quint32(pid) : 0;
}

double gpuPercentForPids(const QVector<GpuEngineReading> &readings, const QSet<quint32> &pids)
{
    if (pids.isEmpty() || readings.isEmpty()) {
        return 0;
    }

    double peak = 0;
    bool any = false;
    for (const GpuEngineReading &reading : readings) {
        if (reading.pid == 0 || !pids.contains(reading.pid)) {
            continue;
        }
        any = true;
        if (reading.utilization > peak) {
            peak = reading.utilization;
        }
    }
    if (!any) {
        return 0;
    }
    if (peak < 0) {
        return 0;
    }
    if (peak > 100) {
        return 100;
    }
    return peak;
}
