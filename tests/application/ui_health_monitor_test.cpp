#include "application/window_transition_coordinator.h"
#include "monitoring/resource_monitor.h"
#include "application/diagnostics/ui_health_monitor.h"

#include <QElapsedTimer>
#include <QFile>
#include <QQuickWindow>
#include <QSemaphore>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>

#include <atomic>

using span::application::WindowTransitionCoordinator;
using span::application::diagnostics::UiHealthMonitor;

namespace {

bool fileContains(const QString &path, const QByteArray &needle)
{
    QFile file(path);
    return file.open(QIODevice::ReadOnly) && file.readAll().contains(needle);
}

QVector<ResourceSampleProvider::SessionTarget> noSessions()
{
    return {};
}

class SlowResourceSampleProvider final : public ResourceSampleProvider {
public:
    Result sample(const QVector<SessionTarget> &,
                  const QHash<QString, SessionSample> &,
                  qint64) override
    {
        calls.fetch_add(1, std::memory_order_release);
        release.acquire();
        return {};
    }

    std::atomic_int calls{0};
    QSemaphore release;
};

class CountingResourceSampleProvider final : public ResourceSampleProvider {
public:
    Result sample(const QVector<SessionTarget> &,
                  const QHash<QString, SessionSample> &,
                  qint64) override
    {
        calls.fetch_add(1, std::memory_order_release);
        return {};
    }

    std::atomic_int calls{0};
};

} // namespace

class UiHealthMonitorTest final : public QObject {
    Q_OBJECT

private slots:
    void oneReportPerStallAndNewReportAfterRecovery();
    void slowResourceSamplerDoesNotReenterOrBlockGui();
    void idleBackoffMultipliesIntervalAndWakesWithRefresh();
};

void UiHealthMonitorTest::oneReportPerStallAndNewReportAfterRecovery()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    QQuickWindow window;
    window.resize(900, 600);
    WindowTransitionCoordinator transition;
    transition.attachWindow(&window);
    UiHealthMonitor monitor(&transition, nullptr, nullptr, nullptr, &window,
                            directory.path(), 250);
    monitor.start();

    QTest::qSleep(650);
    QFile report(directory.filePath(QStringLiteral("hang-latest.log")));
    QTRY_VERIFY_WITH_TIMEOUT(report.exists(), 1000);
    QVERIFY(report.open(QIODevice::ReadOnly));
    QByteArray first = report.readAll();
    report.close();
    QCOMPARE(first.count("===== SpanTui UI stall ====="), 1);
    QVERIFY(first.contains("stallEpoch=1"));
    QVERIFY(first.contains("persist.queueDepth=0"));
    QVERIFY(first.contains("monitor.pdhStatus=0"));
    QVERIFY(first.contains("dump=skipped-in-process"));

    QTRY_VERIFY_WITH_TIMEOUT(fileContains(report.fileName(), "recovered stallEpoch=1"), 1000);

    QTest::qSleep(650);
    QTRY_VERIFY_WITH_TIMEOUT(report.exists(), 1000);
    QVERIFY(report.open(QIODevice::ReadOnly));
    const QByteArray second = report.readAll();
    QCOMPARE(second.count("===== SpanTui UI stall ====="), 1);
    QVERIFY(second.contains("stallEpoch=2"));
}

void UiHealthMonitorTest::slowResourceSamplerDoesNotReenterOrBlockGui()
{
    const auto provider = std::make_shared<SlowResourceSampleProvider>();
    auto *monitor = new ResourceMonitor(noSessions, nullptr, provider);
    monitor->setIntervalSeconds(1);
    monitor->setActive(true);
    QTRY_COMPARE_WITH_TIMEOUT(provider->calls.load(std::memory_order_acquire),
                              1, 1000);

    bool guiHeartbeat = false;
    QTimer::singleShot(0, this, [&guiHeartbeat]() { guiHeartbeat = true; });
    QTRY_VERIFY_WITH_TIMEOUT(guiHeartbeat, 100);
    // Waking asks for a sample at once, like a timer tick, without waiting
    // out the one-second minimum interval; the in-flight one blocks it.
    monitor->setIdle(true);
    monitor->setIdle(false);
    QTest::qWait(100);
    QVERIFY(monitor->samplingInProgress());
    QCOMPARE(provider->calls.load(std::memory_order_acquire), 1);

    QElapsedTimer destruction;
    destruction.start();
    delete monitor;
    QVERIFY2(destruction.elapsed() < 100,
             "ResourceMonitor destruction waited for the sampler");
    provider->release.release();
}

void UiHealthMonitorTest::idleBackoffMultipliesIntervalAndWakesWithRefresh()
{
    const auto provider = std::make_shared<CountingResourceSampleProvider>();
    ResourceMonitor monitor(noSessions, nullptr, provider);
    monitor.setIntervalSeconds(2);
    monitor.setActive(true);
    QCOMPARE(monitor.effectiveIntervalMs(), 2000);
    QCOMPARE(monitor.idle(), false);

    monitor.setIdle(true);
    QCOMPARE(monitor.idle(), true);
    QCOMPARE(monitor.effectiveIntervalMs(), 10000);

    // Let the activation's initial sample settle, otherwise the wake
    // refresh is correctly dropped by the single-in-flight guard.
    QTRY_VERIFY_WITH_TIMEOUT(!monitor.samplingInProgress(), 1000);
    const int before = provider->calls.load(std::memory_order_acquire);
    monitor.setIdle(false);
    QCOMPARE(monitor.effectiveIntervalMs(), 2000);
    // Waking resamples immediately instead of waiting out the next tick.
    QTRY_VERIFY_WITH_TIMEOUT(
        provider->calls.load(std::memory_order_acquire) > before, 500);
}

QTEST_MAIN(UiHealthMonitorTest)
#include "ui_health_monitor_test.moc"
