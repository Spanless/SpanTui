#include "application/terminal_resource_targets.h"
#include "monitoring/resource_monitor.h"
#include "terminal/api/terminal_manager.h"
#include "terminal/api/terminal_session.h"

#include <QDir>
#include <QSignalSpy>
#include <QTest>

using span::terminal::TerminalManager;

class TerminalResourceTargetsTest final : public QObject {
    Q_OBJECT

private slots:
    void targetsDescribeEachSession();
    void platformSamplerMeasuresRunningSessionsOnly();
};

namespace {

// The production sampler against a real shell: the running session's process
// tree has memory, the never-started session reports nothing.
void startShellAndIdleSession(TerminalManager &terminals)
{
    QVERIFY(terminals.createSession(QStringLiteral("shell"), QDir::homePath()));
    QVERIFY(terminals.createSession(QStringLiteral("stopped"), QDir::homePath()));
    terminals.setActiveSession(QStringLiteral("shell"));
    QTRY_VERIFY(terminals.session(QStringLiteral("shell"))->running());
    QVERIFY(!terminals.session(QStringLiteral("stopped"))->running());
}

} // namespace

void TerminalResourceTargetsTest::targetsDescribeEachSession()
{
    TerminalManager terminals;
    startShellAndIdleSession(terminals);
    if (QTest::currentTestFailed()) return;

    const QVector<ResourceSampleProvider::SessionTarget> targets = terminalResourceTargets(terminals);
    QCOMPARE(targets.size(), 2);
    for (const ResourceSampleProvider::SessionTarget &target : targets) {
        if (target.id == QLatin1String("shell")) {
            QVERIFY(target.running);
            QVERIFY(target.rootPid != 0);
        } else {
            QCOMPARE(target.id, QStringLiteral("stopped"));
            QVERIFY(!target.running);
            QCOMPARE(target.rootPid, quint32(0));
            QVERIFY(target.jobPids.isEmpty());
        }
    }
}

void TerminalResourceTargetsTest::platformSamplerMeasuresRunningSessionsOnly()
{
    TerminalManager terminals;
    startShellAndIdleSession(terminals);
    if (QTest::currentTestFailed()) return;

    ResourceMonitor monitor([&terminals] { return terminalResourceTargets(terminals); });
    QSignalSpy samples(&monitor, &ResourceMonitor::metricsChanged);
    monitor.setActive(true);
    QTRY_VERIFY_WITH_TIMEOUT(samples.count() >= 1, 15000);
    // The second sample is the first with a CPU baseline to diff against;
    // waking takes it at once instead of after the interval.
    QTRY_VERIFY(!monitor.samplingInProgress());
    monitor.setIdle(true);
    monitor.setIdle(false);
    QTRY_VERIFY_WITH_TIMEOUT(samples.count() >= 2, 15000);

    const QVariantMap shell = monitor.sessionMetrics(QStringLiteral("shell"));
    QVERIFY(shell.value(QStringLiteral("privateBytes")).toLongLong() > 0);
    QVERIFY(shell.value(QStringLiteral("workingSetBytes")).toLongLong() > 0);
    QCOMPARE(shell.value(QStringLiteral("memoryBytes")), shell.value(QStringLiteral("workingSetBytes")));
    const double shellCpu = shell.value(QStringLiteral("cpuPercent")).toDouble();
    QVERIFY(shellCpu >= 0.0 && shellCpu <= 100.0);
    QVERIFY(monitor.cpuPercent() >= 0.0 && monitor.cpuPercent() <= 100.0);
    QVERIFY(monitor.pdhStatus() != int(ResourceSampleProvider::PdhStatus::ProviderFailed));
    // Installed RAM scales the header's memory load level.
    QVERIFY(monitor.physicalMemoryBytes() > 0);

    const QVariantMap stopped = monitor.sessionMetrics(QStringLiteral("stopped"));
    QVERIFY(stopped.contains(QStringLiteral("memoryBytes")));
    QCOMPARE(stopped.value(QStringLiteral("memoryBytes")).toLongLong(), 0);
    QCOMPARE(stopped.value(QStringLiteral("cpuPercent")).toDouble(), 0.0);

    QVERIFY(monitor.sessionMetrics(QStringLiteral("missing")).isEmpty());
}

QTEST_MAIN(TerminalResourceTargetsTest)
#include "terminal_resource_targets_test.moc"
