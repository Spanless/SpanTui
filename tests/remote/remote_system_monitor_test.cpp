#include "remote/remote_shell_channel.h"
#include "remote/remote_system_monitor.h"

#include <QDir>
#include <QFileInfo>
#include <QSignalSpy>
#include <QStandardPaths>
#include <QTest>

using namespace span::remote;

class RemoteSystemMonitorTest final : public QObject {
    Q_OBJECT

private slots:
    void parsesALinuxReading();
    void parsesOlderKernelsWithoutOptionalSections();
    void rejectsOutputWithoutCpuLine();
    void samplesARealShellOnlyWhileVisible();
    void leavesAFailedChannelDown();
};

namespace {

QString gitShell(QString *binDirectory)
{
    const QString git = QStandardPaths::findExecutable(QStringLiteral("git"));
    if (git.isEmpty()) {
        return {};
    }
    QDir dir = QFileInfo(git).absoluteDir();
    for (int depth = 0; depth < 3; ++depth) {
        const QString shell = dir.filePath(QStringLiteral("usr/bin/sh.exe"));
        if (QFileInfo(shell).isFile()) {
            *binDirectory = QDir::toNativeSeparators(dir.filePath(QStringLiteral("usr/bin")));
            return shell;
        }
        if (!dir.cdUp()) {
            break;
        }
    }
    return {};
}

} // namespace

void RemoteSystemMonitorTest::parsesALinuxReading()
{
    const QByteArray payload = "cpu  100 5 50 800 20 3 2 10 7 0\n"
                               "MemTotal:        4000000 kB\n"
                               "MemFree:          500000 kB\n"
                               "MemAvailable:    1500000 kB\n"
                               "Buffers:          100000 kB\n"
                               "Cached:           900000 kB\n"
                               "net     lo: 9999 10 0 0 0 0 0 0 9999 10 0 0 0 0 0 0\n"
                               "net   eth0: 1000 10 0 0 0 0 0 0 2000 20 0 0 0 0 0 0\n"
                               "net ens4:300 1 0 0 0 0 0 0 400 2 0 0 0 0 0 0\n"
                               "net docker0: 7777 1 0 0 0 0 0 0 7777 1 0 0 0 0 0 0\n"
                               "net vethab12: 5555 1 0 0 0 0 0 0 5555 1 0 0 0 0 0 0\n"
                               "df /dev/vda1 41152736 10288184 29061436 27% /\n"
                               "load 0.15 0.10 0.05 1/123 4567\n";
    const std::optional<RemoteSystemSample> sample = parseRemoteSystemSample(payload);
    QVERIFY(sample.has_value());
    // user..steal; guest is already counted in user.
    QCOMPARE(sample->cpuTotal, quint64(100 + 5 + 50 + 800 + 20 + 3 + 2 + 10));
    QCOMPARE(sample->cpuIdle, quint64(820));
    QCOMPARE(sample->memoryTotalBytes, qint64(4000000) * 1024);
    QCOMPARE(sample->memoryAvailableBytes, qint64(1500000) * 1024);
    // Loopback, container bridges and veth pairs are not counted.
    QCOMPARE(sample->rxBytes, qint64(1300));
    QCOMPARE(sample->txBytes, qint64(2400));
    QCOMPARE(sample->diskTotalBytes, qint64(41152736) * 1024);
    QCOMPARE(sample->diskUsedBytes, qint64(10288184) * 1024);
    QCOMPARE(sample->loadAverage, QStringLiteral("0.15 0.10 0.05"));
}

void RemoteSystemMonitorTest::parsesOlderKernelsWithoutOptionalSections()
{
    const QByteArray payload = "cpu 10 0 10 80\n"
                               "MemTotal: 1000 kB\n"
                               "MemFree: 100 kB\n"
                               "Buffers: 50 kB\n"
                               "Cached: 250 kB\n";
    const std::optional<RemoteSystemSample> sample = parseRemoteSystemSample(payload);
    QVERIFY(sample.has_value());
    QCOMPARE(sample->cpuTotal, quint64(100));
    QCOMPARE(sample->cpuIdle, quint64(80));
    // No MemAvailable (before Linux 3.14): free + buffers + cache.
    QCOMPARE(sample->memoryAvailableBytes, qint64(400) * 1024);
    QCOMPARE(sample->rxBytes, qint64(-1));
    QCOMPARE(sample->diskTotalBytes, qint64(-1));
    QVERIFY(sample->loadAverage.isEmpty());
}

void RemoteSystemMonitorTest::rejectsOutputWithoutCpuLine()
{
    QVERIFY(!parseRemoteSystemSample("MemTotal: 1000 kB\n").has_value());
    QVERIFY(!parseRemoteSystemSample("").has_value());
}

void RemoteSystemMonitorTest::samplesARealShellOnlyWhileVisible()
{
    QString binDirectory;
    const QString shell = gitShell(&binDirectory);
    if (shell.isEmpty()) QSKIP("Git for Windows sh.exe is not available");

    RemoteShellChannel channel;
    channel.setLaunch(shell, {},
                      {{QStringLiteral("PATH"), binDirectory + QLatin1Char(';') + qEnvironmentVariable("PATH")}});
    RemoteSystemMonitor monitor(&channel);
    monitor.setInterval(50);
    QSignalSpy samples(&monitor, &RemoteSystemMonitor::sampleChanged);

    // Nothing runs until a view shows the figures.
    QTest::qWait(150);
    QCOMPARE(channel.state(), RemoteShellChannel::State::Idle);
    QVERIFY(!monitor.active());

    QObject consumer;
    monitor.setConsumerVisible(&consumer, true);
    QVERIFY(monitor.active());
    // Two readings give a CPU percentage; MSYS emulates /proc/stat and
    // /proc/meminfo.
    QTRY_VERIFY_WITH_TIMEOUT(samples.size() >= 2, 10000);
    QVERIFY(monitor.available());
    QVERIFY(monitor.memoryTotalBytes() > 0);
    QVERIFY(monitor.memoryUsedBytes() >= 0);
    QVERIFY(monitor.memoryUsedBytes() <= monitor.memoryTotalBytes());
    QVERIFY(monitor.cpuPercent() >= 0);
    QVERIFY(monitor.cpuPercent() <= 100);
    QVERIFY(monitor.diskTotalBytes() > 0);

    monitor.setConsumerVisible(&consumer, false);
    QVERIFY(!monitor.active());
    QTest::qWait(150);
    const qsizetype settled = samples.size();
    QTest::qWait(200);
    QCOMPARE(samples.size(), settled);
}

void RemoteSystemMonitorTest::leavesAFailedChannelDown()
{
    RemoteShellChannel channel;
    channel.setLaunch(QDir(QCoreApplication::applicationDirPath()).filePath(QStringLiteral("missing-ssh-for-test.exe")),
                      {}, {});
    RemoteSystemMonitor monitor(&channel);
    monitor.setInterval(20);
    QSignalSpy failures(&channel, &RemoteShellChannel::requestFailed);

    QObject consumer;
    monitor.setConsumerVisible(&consumer, true);
    QTRY_COMPARE(failures.size(), 1);
    QCOMPARE(channel.state(), RemoteShellChannel::State::Failed);
    // Polling a failed channel would relaunch ssh (and its password prompt)
    // every interval.
    QTest::qWait(200);
    QCOMPARE(failures.size(), 1);
    QVERIFY(!monitor.available());
}

QTEST_GUILESS_MAIN(RemoteSystemMonitorTest)
#include "remote_system_monitor_test.moc"
