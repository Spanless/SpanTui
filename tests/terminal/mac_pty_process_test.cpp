#include "terminal/platform/macos/mac_pty_process.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>
#include <QTemporaryDir>
#include <QTest>

#include <chrono>
#include <condition_variable>
#include <mutex>

#include <signal.h>

namespace {
using span::terminal::MacPtyProcess;
using namespace std::chrono_literals;

constexpr auto kTimeout = 10s;

struct ProcessEvents {
    QByteArray output;
    bool exited = false;
    quint32 exitCode = 0;
    std::mutex mutex;
    std::condition_variable changed;

    MacPtyProcess::OutputCallback outputCallback()
    {
        return [this](const QByteArray &bytes, bool) {
            {
                std::lock_guard<std::mutex> lock(mutex);
                output.append(bytes);
            }
            changed.notify_all();
        };
    }

    MacPtyProcess::ExitCallback exitCallback()
    {
        return [this](quint32 code) {
            {
                std::lock_guard<std::mutex> lock(mutex);
                exited = true;
                exitCode = code;
            }
            changed.notify_all();
        };
    }

    bool waitForOutput(const QByteArray &needle)
    {
        std::unique_lock<std::mutex> lock(mutex);
        return changed.wait_for(lock, kTimeout, [&]() { return output.contains(needle); });
    }

    bool waitForExit()
    {
        std::unique_lock<std::mutex> lock(mutex);
        return changed.wait_for(lock, kTimeout, [&]() { return exited; });
    }

    QByteArray received()
    {
        std::lock_guard<std::mutex> lock(mutex);
        return output;
    }
};

MacPtyProcess::StartOptions command(const QString &commandLine)
{
    MacPtyProcess::StartOptions options;
    options.commandLine = commandLine;
    return options;
}
} // namespace

class MacPtyProcessTest final : public QObject {
    Q_OBJECT

private slots:
    void inputEchoesBackThroughTheTerminal();
    void childSeesTheInitialSizeAndLaterResizes();
    void childRunsInTheRequestedDirectory();
    void environmentCarriesTerminalIdentityAndOverrides();
    void childGetsUsableUtf8LocaleForUnknownInheritedOne();
    void exitCodeReachesTheCallbackAfterTheLastOutput();
    void signalDeathReportsShellStyleExitCode();
    void interruptSignalsTheForegroundJob();
    void stopEndsTheChildWithoutAnExitCallback();
    void stopEndsAForegroundJobThatIgnoresTheHangup();
    void refusesMissingExecutable();
    void refusesMissingWorkingDirectory();
};

void MacPtyProcessTest::inputEchoesBackThroughTheTerminal()
{
    ProcessEvents events;
    MacPtyProcess process;
    QString error;
    QVERIFY2(process.start(command(QStringLiteral("/bin/cat")), events.outputCallback(), events.exitCallback(), &error),
             qPrintable(error));
    QVERIFY(process.isRunning());
    QVERIFY(process.processId() != 0);

    QVERIFY(process.write("span round trip\n"));
    QVERIFY2(events.waitForOutput("span round trip"), events.received().constData());
    process.stop();
}

void MacPtyProcessTest::childSeesTheInitialSizeAndLaterResizes()
{
    ProcessEvents events;
    MacPtyProcess process;
    auto options = command(QStringLiteral("/bin/sh -c \"stty size; read line; stty size\""));
    options.columns = 91;
    options.rows = 27;
    QString error;
    QVERIFY2(process.start(options, events.outputCallback(), events.exitCallback(), &error), qPrintable(error));
    QVERIFY2(events.waitForOutput("27 91"), events.received().constData());

    process.requestResize(120, 40);
    QVERIFY(process.write("\n"));
    QVERIFY2(events.waitForOutput("40 120"), events.received().constData());
    QVERIFY(events.waitForExit());
}

void MacPtyProcessTest::childRunsInTheRequestedDirectory()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    ProcessEvents events;
    MacPtyProcess process;
    auto options = command(QStringLiteral("/bin/pwd -P"));
    options.workingDirectory = directory.path();
    QString error;
    QVERIFY2(process.start(options, events.outputCallback(), events.exitCallback(), &error), qPrintable(error));
    QVERIFY(events.waitForExit());
    QVERIFY2(events.received().contains(QFileInfo(directory.path()).canonicalFilePath().toUtf8()),
             events.received().constData());
}

void MacPtyProcessTest::environmentCarriesTerminalIdentityAndOverrides()
{
    ProcessEvents events;
    MacPtyProcess process;
    auto options = command(QStringLiteral("/bin/sh -c \"echo [$TERM][$COLORTERM][$SPAN_TEST_VALUE]\""));
    options.environment.insert(QStringLiteral("SPAN_TEST_VALUE"), QStringLiteral("layered"));
    QString error;
    QVERIFY2(process.start(options, events.outputCallback(), events.exitCallback(), &error), qPrintable(error));
    QVERIFY(events.waitForExit());
    QVERIFY2(events.received().contains("[xterm-256color][truecolor][layered]"), events.received().constData());
}

void MacPtyProcessTest::childGetsUsableUtf8LocaleForUnknownInheritedOne()
{
    // A region-language mix macOS has no locale for, as a shell profile or
    // System Settings may export it.
    const bool hadLang = qEnvironmentVariableIsSet("LANG");
    const QByteArray saved = qgetenv("LANG");
    qputenv("LANG", "zh_US.UTF-8");
    ProcessEvents events;
    MacPtyProcess process;
    QString error;
    const bool started = process.start(command(QStringLiteral("/bin/sh -c \"echo [$(locale charmap)]\"")),
                                       events.outputCallback(), events.exitCallback(), &error);
    if (hadLang) {
        qputenv("LANG", saved);
    } else {
        qunsetenv("LANG");
    }
    QVERIFY2(started, qPrintable(error));
    QVERIFY(events.waitForExit());
    QVERIFY2(events.received().contains("[UTF-8]"), events.received().constData());
}

void MacPtyProcessTest::exitCodeReachesTheCallbackAfterTheLastOutput()
{
    ProcessEvents events;
    MacPtyProcess process;
    QString error;
    QVERIFY2(process.start(command(QStringLiteral("/bin/sh -c \"echo last words; exit 7\"")), events.outputCallback(),
                           events.exitCallback(), &error),
             qPrintable(error));
    QVERIFY(events.waitForExit());
    QCOMPARE(events.exitCode, quint32(7));
    QVERIFY2(events.received().contains("last words"), events.received().constData());
    QVERIFY(!process.isRunning());
}

void MacPtyProcessTest::signalDeathReportsShellStyleExitCode()
{
    ProcessEvents events;
    MacPtyProcess process;
    QString error;
    QVERIFY2(process.start(command(QStringLiteral("/bin/sh -c \"kill -KILL $$\"")), events.outputCallback(),
                           events.exitCallback(), &error),
             qPrintable(error));
    QVERIFY(events.waitForExit());
    QCOMPARE(events.exitCode, quint32(128 + SIGKILL));
}

void MacPtyProcessTest::interruptSignalsTheForegroundJob()
{
    ProcessEvents events;
    MacPtyProcess process;
    QString error;
    QVERIFY2(
        process.start(command(QStringLiteral(
                          "/bin/sh -c \"trap 'echo caught; exit 3' INT; echo ready; while :; do sleep 0.05; done\"")),
                      events.outputCallback(), events.exitCallback(), &error),
        qPrintable(error));
    QVERIFY2(events.waitForOutput("ready"), events.received().constData());

    QVERIFY(process.cancelPendingWritesAndInterrupt());
    QVERIFY2(events.waitForOutput("caught"), events.received().constData());
    QVERIFY(events.waitForExit());
    QCOMPARE(events.exitCode, quint32(3));
}

void MacPtyProcessTest::stopEndsTheChildWithoutAnExitCallback()
{
    ProcessEvents events;
    MacPtyProcess process;
    QString error;
    QVERIFY2(process.start(command(QStringLiteral("/bin/sh -c \"echo ready; exec sleep 30\"")), events.outputCallback(),
                           events.exitCallback(), &error),
             qPrintable(error));
    QVERIFY(events.waitForOutput("ready"));
    const pid_t pid = pid_t(process.processId());

    process.stop();
    QVERIFY(!process.isRunning());
    QCOMPARE(process.processId(), quint32(0));
    // Reaped by stop(): the pid no longer names a process of ours.
    QCOMPARE(::kill(pid, 0), -1);
    QVERIFY(!events.exited);
}

void MacPtyProcessTest::stopEndsAForegroundJobThatIgnoresTheHangup()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString job = directory.filePath(QStringLiteral("job.sh"));
    QFile script(job);
    QVERIFY(script.open(QIODevice::WriteOnly));
    script.write("#!/bin/sh\ntrap '' HUP\necho job=$$ ready\nexec sleep 60\n");
    script.close();
    QVERIFY(script.setPermissions(script.permissions() | QFileDevice::ExeOwner));

    ProcessEvents events;
    MacPtyProcess process;
    QString error;
    // With job control the shell runs the job in a process group of its own
    // and hands it the terminal, as an interactive shell does.
    QVERIFY2(process.start(command(QStringLiteral("/bin/sh -c \"set -m; %1; echo done\"").arg(job)),
                           events.outputCallback(), events.exitCallback(), &error),
             qPrintable(error));
    QVERIFY2(events.waitForOutput(" ready"), events.received().constData());
    const QRegularExpressionMatch match =
        QRegularExpression(QStringLiteral("job=(\\d+) ready")).match(QString::fromLatin1(events.received()));
    QVERIFY2(match.hasMatch(), events.received().constData());
    const pid_t jobPid = pid_t(match.captured(1).toInt());

    process.stop();
    QTRY_VERIFY_WITH_TIMEOUT(::kill(jobPid, 0) == -1, 5000);
}

void MacPtyProcessTest::refusesMissingExecutable()
{
    ProcessEvents events;
    MacPtyProcess process;
    QString error;
    QVERIFY(!process.start(command(QStringLiteral("/nonexistent/span-missing-shell")), events.outputCallback(),
                           events.exitCallback(), &error));
    QVERIFY2(error.contains(QStringLiteral("span-missing-shell")), qPrintable(error));
    QVERIFY(!process.isRunning());
}

void MacPtyProcessTest::refusesMissingWorkingDirectory()
{
    ProcessEvents events;
    MacPtyProcess process;
    auto options = command(QStringLiteral("/bin/cat"));
    options.workingDirectory = QStringLiteral("/nonexistent/span-missing-cwd");
    QString error;
    QVERIFY(!process.start(options, events.outputCallback(), events.exitCallback(), &error));
    QVERIFY2(error.contains(QStringLiteral("Working directory")), qPrintable(error));
}

QTEST_GUILESS_MAIN(MacPtyProcessTest)

#include "mac_pty_process_test.moc"
