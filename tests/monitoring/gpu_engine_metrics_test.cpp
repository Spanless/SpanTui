#include "monitoring/gpu_engine_metrics.h"

#include <QTest>

class GpuEngineMetricsTest final : public QObject {
    Q_OBJECT

private slots:
    void parsesPidFromEngineInstance();
    void rejectsMalformedInstanceNames();
    void peakIgnoresOtherPidsAndClamps();
};

void GpuEngineMetricsTest::parsesPidFromEngineInstance()
{
    QCOMPARE(parseGpuEnginePid(
                 QStringLiteral("pid_4321_luid_0x00000000_0x00013BF4_phys_0_eng_0_engtype_3D")),
             quint32(4321));
    QCOMPARE(parseGpuEnginePid(
                 QStringLiteral("\\GPU Engine(pid_88_luid_0x0_0x1_phys_0_eng_1_engtype_Compute)\\Utilization Percentage")),
             quint32(88));
}

void GpuEngineMetricsTest::rejectsMalformedInstanceNames()
{
    QCOMPARE(parseGpuEnginePid(QString()), quint32(0));
    QCOMPARE(parseGpuEnginePid(QStringLiteral("engtype_3D")), quint32(0));
    QCOMPARE(parseGpuEnginePid(QStringLiteral("pid_")), quint32(0));
    QCOMPARE(parseGpuEnginePid(QStringLiteral("pid_abc")), quint32(0));
    QCOMPARE(parseGpuEnginePid(QStringLiteral("pid_4294967296_luid_0x0")), quint32(0));
}

void GpuEngineMetricsTest::peakIgnoresOtherPidsAndClamps()
{
    const QVector<GpuEngineReading> readings{
        {1001, 12.5},
        {1001, 40.0},
        {2002, 90.0},
        {1001, 140.0},
        {1001, -4.0},
        {0, 50.0},
    };

    QCOMPARE(gpuPercentForPids(readings, {1001}), 100.0);
    QCOMPARE(gpuPercentForPids(readings, {2002}), 90.0);
    QCOMPARE(gpuPercentForPids(readings, {3003}), 0.0);
    QCOMPARE(gpuPercentForPids(readings, {}), 0.0);
    QCOMPARE(gpuPercentForPids({}, {1001}), 0.0);

    const QVector<GpuEngineReading> negative{{7, -12.0}};
    QCOMPARE(gpuPercentForPids(negative, {7}), 0.0);
}

QTEST_APPLESS_MAIN(GpuEngineMetricsTest)
#include "gpu_engine_metrics_test.moc"
