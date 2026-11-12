#include "application/terminal_resource_targets.h"
#include "monitoring/resource_monitor.h"
#include "terminal/api/terminal_manager.h"
#include "terminal/api/terminal_session.h"

#include <QDir>
#include <QSignalSpy>
#include <QTest>

#include <algorithm>

using span::terminal::TerminalManager;

class TerminalResourceTargetsTest final : public QObject {
    Q_OBJECT

private slots:
    void targetsDescribeEachSession();
    void platformSamplerMeasuresRunningSessionsOnly();
};

namespace {

// The production sampler against a real shell: the running session's process
// tree has memory, the never-started session reports nothing. False when the
// shell does not start.
bool startShellAndIdleSession(TerminalManager &terminals)
{
    if (!terminals.createSession(QStringLiteral("shell"), QDir::homePath())
        || !terminals.createSession(QStringLiteral("stopped"), QDir::homePath()))
        return false;
    terminals.setActiveSession(QStringLiteral("shell"));
    return QTest::qWaitFor([&terminals] { return terminals.session(QStringLiteral("shell"))->running(); }, 5000)
        && !terminals.session(QStringLiteral("stopped"))->running();
}

// The target of session `id`, or an empty one.
ResourceSampleProvider::SessionTarget targetOf(const QVector<ResourceSampleProvider::SessionTarget> &targets,
                                               const QString &id)
{
    const auto found =
        std::find_if(targets.cbegin(), targets.cend(),
                     [&id](const ResourceSampleProvider::SessionTarget &target) { return target.id == id; });
    return found == targets.cend() ? ResourceSampleProvider::SessionTarget{} : *found;
}

} // namespace

void TerminalResourceTargetsTest::targetsDescribeEachSession()
{
    TerminalManager terminals;
    QVERIFY(startShellAndIdleSession(terminals));

    const QVector<ResourceSampleProvider::SessionTarget> targets = terminalResourceTargets(terminals);
    QCOMPARE(targets.size(), 2);
    const ResourceSampleProvider::SessionTarget shell = targetOf(targets, QStringLiteral("shell"));
    QVERIFY(shell.running);
    QVERIFY(shell.rootPid != 0);
    const ResourceSampleProvider::SessionTarget stopped = targetOf(targets, QStringLiteral("stopped"));
    QCOMPARE(stopped.id, QStringLiteral("stopped"));
    QVERIFY(!stopped.running);
    QCOMPARE(stopped.rootPid, quint32(0));
    QVERIFY(stopped.jobPids.isEmpty());
}

void TerminalResourceTargetsTest::platformSamplerMeasuresRunningSessionsOnly()
{
    TerminalManager terminals;
    QVERIFY(startShellAndIdleSession(terminals));

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
