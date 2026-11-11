#include "monitoring/resource_monitor.h"

#include <QSignalSpy>
#include <QTest>

#include <atomic>
#include <memory>
#include <mutex>
#include <stdexcept>

namespace {

QVector<ResourceSampleProvider::SessionTarget> noSessions()
{
    return {};
}

class ThrowingResourceSampleProvider final : public ResourceSampleProvider {
public:
    Result sample(const QVector<SessionTarget> &,
                  const QHash<QString, SessionSample> &,
                  qint64) override
    {
        ++calls;
        throw std::runtime_error("sampler failed");
    }

    std::atomic_int calls{0};
};

class RecordingResourceSampleProvider final : public ResourceSampleProvider {
public:
    Result sample(const QVector<SessionTarget> &targets, const QHash<QString, SessionSample> &, qint64) override
    {
        const std::lock_guard lock(mutex);
        seen = targets;
        ++calls;
        return {};
    }

    QVector<SessionTarget> lastTargets()
    {
        const std::lock_guard lock(mutex);
        return seen;
    }

    std::atomic_int calls{0};

private:
    std::mutex mutex;
    QVector<SessionTarget> seen;
};

class CountingResourceSampleProvider final : public ResourceSampleProvider {
public:
    Result sample(const QVector<SessionTarget> &, const QHash<QString, SessionSample> &, qint64) override
    {
        ++calls;
        return {};
    }

    std::atomic_int calls{0};
};

} // namespace

class ResourceMonitorTest final : public QObject {
    Q_OBJECT

private slots:
    void sessionTargetsComeFromTheSourceEachRound();
    void providerFailureIsReportedAndSamplingContinues();
    void inactiveMonitorDoesNotSample();
    void visibleConsumersDriveActivation();
};

void ResourceMonitorTest::sessionTargetsComeFromTheSourceEachRound()
{
    const auto provider = std::make_shared<RecordingResourceSampleProvider>();
    int asked = 0;
    ResourceMonitor monitor(
        [&asked] {
            ++asked;
            ResourceSampleProvider::SessionTarget target;
            target.id = QStringLiteral("a");
            target.rootPid = 4242;
            target.jobPids = {4243};
            target.running = true;
            return QVector<ResourceSampleProvider::SessionTarget>{target};
        },
        nullptr, provider);
    monitor.setActive(true);
    QTRY_COMPARE(provider->calls.load(), 1);
    QCOMPARE(asked, 1);
    const QVector<ResourceSampleProvider::SessionTarget> targets = provider->lastTargets();
    QCOMPARE(targets.size(), 1);
    QCOMPARE(targets.first().id, QStringLiteral("a"));
    QCOMPARE(targets.first().rootPid, quint32(4242));
    QCOMPARE(targets.first().jobPids, QList<quint32>{4243});
    QVERIFY(targets.first().running);

    // Waking resamples at once, and the source is asked again.
    QTRY_VERIFY(!monitor.samplingInProgress());
    monitor.setIdle(true);
    monitor.setIdle(false);
    QTRY_COMPARE(provider->calls.load(), 2);
    QCOMPARE(asked, 2);
}

void ResourceMonitorTest::providerFailureIsReportedAndSamplingContinues()
{
    const auto provider = std::make_shared<ThrowingResourceSampleProvider>();
    ResourceMonitor monitor(noSessions, nullptr, provider);
    QSignalSpy samples(&monitor, &ResourceMonitor::metricsChanged);
    monitor.setActive(true);
    QTRY_COMPARE(samples.count(), 1);
    QCOMPARE(monitor.pdhStatus(), int(ResourceSampleProvider::PdhStatus::ProviderFailed));
    QVERIFY(!monitor.samplingInProgress());

    // A failed sample must not leave the single-in-flight guard set.
    monitor.setIdle(true);
    monitor.setIdle(false);
    QTRY_COMPARE(samples.count(), 2);
    QCOMPARE(provider->calls.load(), 2);
}

void ResourceMonitorTest::inactiveMonitorDoesNotSample()
{
    const auto provider = std::make_shared<CountingResourceSampleProvider>();
    ResourceMonitor monitor(noSessions, nullptr, provider);
    monitor.setIntervalSeconds(1);
    QVERIFY(!monitor.active());
    // Neither construction, the interval tick nor waking from idle samples.
    monitor.setIdle(true);
    monitor.setIdle(false);
    QTest::qWait(1300);
    QCOMPARE(provider->calls.load(), 0);

    // Activation samples at once; deactivation stops the ticks.
    monitor.setActive(true);
    QTRY_COMPARE_WITH_TIMEOUT(provider->calls.load(), 1, 1000);
    QTRY_VERIFY(!monitor.samplingInProgress());
    monitor.setActive(false);
    const int calls = provider->calls.load();
    QTest::qWait(1300);
    QCOMPARE(provider->calls.load(), calls);
}

void ResourceMonitorTest::visibleConsumersDriveActivation()
{
    const auto provider = std::make_shared<CountingResourceSampleProvider>();
    ResourceMonitor monitor(noSessions, nullptr, provider);
    QObject first;
    auto *second = new QObject;

    monitor.setConsumerVisible(&first, false);
    QVERIFY(!monitor.active());
    monitor.setConsumerVisible(&first, true);
    monitor.setConsumerVisible(second, true);
    QVERIFY(monitor.active());
    QTRY_COMPARE(provider->calls.load(), 1);

    // Active while any consumer is visible; a destroyed one stops counting.
    monitor.setConsumerVisible(&first, false);
    QVERIFY(monitor.active());
    delete second;
    QVERIFY(!monitor.active());

    // Visibility is a set per consumer, not a reference count.
    monitor.setConsumerVisible(&first, true);
    monitor.setConsumerVisible(&first, true);
    monitor.setConsumerVisible(&first, false);
    QVERIFY(!monitor.active());
}

QTEST_MAIN(ResourceMonitorTest)
#include "resource_monitor_test.moc"
