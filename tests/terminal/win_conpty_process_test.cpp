#include "terminal/platform/windows/win_conpty_process.h"

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTest>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <tlhelp32.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>

namespace {
using span::terminal::WinConPtyProcess;

QString quoteCommand(const QString &path) {
    return QStringLiteral("\"%1\"").arg(QDir::toNativeSeparators(path));
}

struct ProcessEvents {
    QByteArray output;
    bool exited = false;
    quint32 exitCode = 0;
    std::mutex mutex;
    std::condition_variable changed;

    void append(const QByteArray &bytes) {
        {
            std::lock_guard<std::mutex> lock(mutex);
            output.append(bytes);
        }
        changed.notify_all();
    }

    void finish(quint32 code) {
        {
            std::lock_guard<std::mutex> lock(mutex);
            exited = true;
            exitCode = code;
        }
        changed.notify_all();
    }

    bool waitForOutput(const QByteArray &needle, std::chrono::milliseconds timeout) {
        std::unique_lock<std::mutex> lock(mutex);
        return changed.wait_for(lock, timeout, [&]() {
            return output.contains(needle);
        });
    }

    bool waitForChildReady(std::chrono::milliseconds timeout) {
        std::unique_lock<std::mutex> lock(mutex);
        return changed.wait_for(lock, timeout, [&]() {
            // ConPTY may insert its initial OSC title update after the first
            // painted character of a console write. The rendered line is
            // still "span_child_ready", but the raw transport can contain
            // `s` + OSC + `pan_child_ready`.
            return output.contains("span_child_ready") || output.contains("pan_child_ready");
        });
    }

    bool waitForExit(std::chrono::milliseconds timeout) {
        std::unique_lock<std::mutex> lock(mutex);
        return changed.wait_for(lock, timeout, [&]() {
            return exited;
        });
    }

    QByteArray diagnostic() {
        std::lock_guard<std::mutex> lock(mutex);
        return QByteArray("exited=") + (exited ? "1" : "0") + " code=0x"
            + QByteArray::number(exitCode, 16).rightJustified(8, '0') + " output=" + output.toHex(' ');
    }
};

// Starts pty_echo_child in interrupt-bytes mode and waits until it reads
// raw input. Empty on success, otherwise why not.
QString startInterruptBytesChild(WinConPtyProcess &process, ProcessEvents &events, const QString &directory)
{
    const QString childPath =
        QDir(QCoreApplication::applicationDirPath()).filePath(QStringLiteral("pty_echo_child.exe"));
    WinConPtyProcess::StartOptions options;
    options.commandLine = quoteCommand(childPath) + QStringLiteral(" interrupt-bytes");
    options.workingDirectory = directory;
    QString error;
    if (!process.start(
            options, [&](const QByteArray &bytes, bool) { events.append(bytes); },
            [&](quint32 code) { events.finish(code); }, &error))
        return error;
    if (!events.waitForOutput("span_interrupt_bytes_ready", std::chrono::seconds(8)))
        return QString::fromLatin1(events.diagnostic());
    return {};
}
}

class WinConPtyProcessTest final : public QObject {
    Q_OBJECT

private slots:
    void deterministicChildRoundTrip();
    void shellRoundTrip();
    void exportsColorFgBgAndEnvironmentFromStartOptions();
    void refusesMissingDirectoryOrProgram();
    void defaultShellRoundTrip();
    void resizePseudoConsole();
    void interruptStopsForegroundCommand();
    void interruptWithoutPasteSendsOnlyCtrlC();
    void interruptClosesDeliveredPaste();
    void stopDoesNotHang();
    void sessionJobOwnsConsoleHostAndForegroundChild();
    void buildTreeUsesBundledConPty();
};

void WinConPtyProcessTest::deterministicChildRoundTrip() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());

    const QString childPath = QDir(QCoreApplication::applicationDirPath())
                                  .filePath(QStringLiteral("pty_echo_child.exe"));
    QVERIFY2(QFileInfo::exists(childPath), qPrintable(childPath));

    ProcessEvents events;
    WinConPtyProcess process;
    WinConPtyProcess::StartOptions options;
    options.commandLine = quoteCommand(childPath);
    options.workingDirectory = directory.path();

    QString error;
    QVERIFY2(process.start(options,
                           [&](const QByteArray &bytes, bool) { events.append(bytes); },
                           [&](quint32 code) { events.finish(code); },
                           &error),
             qPrintable(error));

    QVERIFY2(events.waitForChildReady(std::chrono::seconds(8)), events.diagnostic().constData());
    QVERIFY2(events.waitForOutput("span_child_console=1", std::chrono::seconds(2)),
             events.diagnostic().constData());
    QVERIFY(process.write(QByteArrayLiteral("hello from pty\r")));
    QVERIFY2(events.waitForOutput("span_child_echo=hello from pty", std::chrono::seconds(8)),
             events.diagnostic().constData());
    QVERIFY(process.write(QByteArrayLiteral("exit\r")));
    QVERIFY(events.waitForExit(std::chrono::seconds(8)));
    QCOMPARE(events.exitCode, quint32(7));
    process.stop();
}

void WinConPtyProcessTest::shellRoundTrip() {
    const QString cmd = QStandardPaths::findExecutable(QStringLiteral("cmd.exe"));
    QVERIFY(!cmd.isEmpty());

    QTemporaryDir directory;
    QVERIFY(directory.isValid());

    ProcessEvents events;
    WinConPtyProcess process;
    WinConPtyProcess::StartOptions options;
    options.commandLine = quoteCommand(cmd) + QStringLiteral(" /d /q");
    options.workingDirectory = directory.path();

    QString error;
    QVERIFY2(process.start(options,
                           [&](const QByteArray &bytes, bool) { events.append(bytes); },
                           [&](quint32 code) { events.finish(code); },
                           &error),
             qPrintable(error));

    QVERIFY(process.write(QByteArrayLiteral("echo span_pty_ok\r")));
    QVERIFY2(events.waitForOutput("span_pty_ok", std::chrono::seconds(8)),
             events.diagnostic().constData());
    QVERIFY(process.write(QByteArrayLiteral("echo %TERM%-%COLORTERM%-%TERM_PROGRAM%\r")));
    QVERIFY2(events.waitForOutput("xterm-kitty-truecolor-SpanTui", std::chrono::seconds(8)),
             events.diagnostic().constData());
    QVERIFY(process.write(QByteArrayLiteral("echo %COLORFGBG%\r")));
    QVERIFY2(events.waitForOutput("15;0", std::chrono::seconds(8)),
             events.diagnostic().constData());
    QVERIFY(process.write(QByteArrayLiteral(
        "if defined WT_SESSION (echo span_wt_session_leaked) else (echo span_wt_session_absent)\r")));
    QVERIFY2(events.waitForOutput("span_wt_session_absent", std::chrono::seconds(8)),
             events.diagnostic().constData());
    QVERIFY(process.write(QByteArrayLiteral("exit\r")));
    QVERIFY(events.waitForExit(std::chrono::seconds(8)));
    QCOMPARE(events.exitCode, quint32(0));
    process.stop();
}

void WinConPtyProcessTest::exportsColorFgBgAndEnvironmentFromStartOptions() {
    const QString cmd = QStandardPaths::findExecutable(QStringLiteral("cmd.exe"));
    QVERIFY(!cmd.isEmpty());

    QTemporaryDir directory;
    QVERIFY(directory.isValid());

    ProcessEvents events;
    WinConPtyProcess process;
    WinConPtyProcess::StartOptions options;
    options.commandLine = quoteCommand(cmd) + QStringLiteral(" /d /q");
    options.workingDirectory = directory.path();
    options.colorFgBg = QStringLiteral("0;15");
    options.environment.insert(QStringLiteral("SPAN_LAYERED"), QStringLiteral("from-options"));

    QString error;
    QVERIFY2(process.start(options,
                           [&](const QByteArray &bytes, bool) { events.append(bytes); },
                           [&](quint32 code) { events.finish(code); },
                           &error),
             qPrintable(error));

    QVERIFY(process.write(QByteArrayLiteral("echo %COLORFGBG% %SPAN_LAYERED%\r")));
    QVERIFY2(events.waitForOutput("0;15 from-options", std::chrono::seconds(8)),
             events.diagnostic().constData());
    QVERIFY(process.write(QByteArrayLiteral("exit\r")));
    QVERIFY(events.waitForExit(std::chrono::seconds(8)));
    process.stop();
}

void WinConPtyProcessTest::refusesMissingDirectoryOrProgram() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const auto refusal = [](const WinConPtyProcess::StartOptions &options) {
        WinConPtyProcess process;
        QString error;
        const bool started = process.start(options, [](const QByteArray &, bool) {},
                                           [](quint32) {}, &error);
        process.stop();
        return started ? QString() : error;
    };

    WinConPtyProcess::StartOptions options;
    options.commandLine = QStringLiteral("cmd.exe /d /c exit");
    options.workingDirectory = directory.filePath(QStringLiteral("gone"));
    QVERIFY(refusal(options).contains(QStringLiteral("Working directory is missing")));

    options.commandLine = quoteCommand(directory.filePath(QStringLiteral("no-such-shell.exe")));
    options.workingDirectory = directory.path();
    QVERIFY(refusal(options).startsWith(QStringLiteral("Could not start shell:")));
}

void WinConPtyProcessTest::defaultShellRoundTrip() {
    ProcessEvents events;
    WinConPtyProcess process;
    WinConPtyProcess::StartOptions options;
    options.commandLine = WinConPtyProcess::defaultShellCommand();
    options.workingDirectory = QCoreApplication::applicationDirPath();

    QString error;
    QVERIFY2(process.start(options,
                           [&](const QByteArray &bytes, bool) { events.append(bytes); },
                           [&](quint32 code) { events.finish(code); },
                           &error),
             qPrintable(error));
    QVERIFY(process.write(QByteArrayLiteral("pwd\rdir\rClear-Host\recho span_default_shell_ok\r")));
    QVERIFY2(events.waitForOutput("span_default_shell_ok", std::chrono::seconds(10)),
             events.diagnostic().constData());
    QVERIFY2(events.waitForOutput("SpanTui-test.exe", std::chrono::seconds(5)),
             events.diagnostic().constData());
    QVERIFY2(events.waitForOutput("\x1b[2J", std::chrono::seconds(5)),
             events.diagnostic().constData());
    QVERIFY(process.write(QByteArrayLiteral("exit\r")));
    QVERIFY(events.waitForExit(std::chrono::seconds(10)));
    process.stop();
}

void WinConPtyProcessTest::resizePseudoConsole() {
    const QString childPath = QDir(QCoreApplication::applicationDirPath())
                                  .filePath(QStringLiteral("pty_echo_child.exe"));
    QVERIFY2(QFileInfo::exists(childPath), qPrintable(childPath));

    ProcessEvents events;
    WinConPtyProcess process;
    WinConPtyProcess::StartOptions options;
    options.commandLine = quoteCommand(childPath);
    options.columns = 40;
    options.rows = 12;

    QString error;
    QVERIFY2(process.start(options,
                           [&](const QByteArray &bytes, bool) { events.append(bytes); },
                           [&](quint32 code) { events.finish(code); },
                           &error),
             qPrintable(error));

    QVERIFY2(events.waitForChildReady(std::chrono::seconds(8)),
             events.diagnostic().constData());
    QVERIFY(process.write(QByteArrayLiteral("size\r")));
    QVERIFY2(events.waitForOutput("span_child_size=40x12", std::chrono::seconds(8)),
             events.diagnostic().constData());
    // Only the latest of several queued sizes must reach the child.
    process.requestResize(60, 15);
    process.requestResize(72, 18);
    // The resize completes on the thread pool: ask until the child sees it.
    QVERIFY(QTest::qWaitFor(
        [&]() {
            process.write(QByteArrayLiteral("size\r"));
            return events.waitForOutput("span_child_size=72x18", std::chrono::milliseconds(500));
        },
        8000));
    QVERIFY(process.write(QByteArrayLiteral("exit\r")));
    QVERIFY(events.waitForExit(std::chrono::seconds(8)));
    process.stop();
}

void WinConPtyProcessTest::interruptStopsForegroundCommand() {
    // Product interrupt is stream ETX (0x03), matching Windows Terminal.
    // Control-event synthesis by ConPTY depends on host console mode and is
    // not guaranteed for every child; assert the API is non-blocking and the
    // session stays healthy. When the host does deliver CTRL_C_EVENT, also
    // assert the child handler observed it.
    const QString childPath = QDir(QCoreApplication::applicationDirPath())
                                  .filePath(QStringLiteral("pty_echo_child.exe"));
    QVERIFY2(QFileInfo::exists(childPath), qPrintable(childPath));

    ProcessEvents events;
    WinConPtyProcess process;
    WinConPtyProcess::StartOptions options;
    options.commandLine = quoteCommand(childPath);

    QString error;
    QVERIFY2(process.start(options,
                           [&](const QByteArray &bytes, bool) { events.append(bytes); },
                           [&](quint32 code) { events.finish(code); },
                           &error),
             qPrintable(error));
    QVERIFY2(events.waitForChildReady(std::chrono::seconds(8)),
             events.diagnostic().constData());
    QVERIFY(process.write(QByteArrayLiteral("block\r")));
    QVERIFY2(events.waitForOutput("span_child_blocking", std::chrono::seconds(8)),
             events.diagnostic().constData());
    QTest::qWait(200);
    QElapsedTimer timer;
    timer.start();
    QVERIFY(process.cancelPendingWritesAndInterrupt());
    QVERIFY2(timer.elapsed() < 2000, "Interrupt blocked the caller.");
    QVERIFY(process.isRunning());
    // The process stops with the fixture when the host cannot deliver.
    if (!events.waitForOutput("span_child_interrupted", std::chrono::seconds(3)))
        QSKIP("ConPTY did not synthesize CTRL_C_EVENT for this host; "
              "stream interrupt remained non-blocking and left the session running.");
    QVERIFY(process.write(QByteArrayLiteral("exit\r")));
    QVERIFY(events.waitForExit(std::chrono::seconds(8)));
    QCOMPARE(events.exitCode, quint32(7));
    process.stop();
}

void WinConPtyProcessTest::interruptWithoutPasteSendsOnlyCtrlC()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    ProcessEvents events;
    WinConPtyProcess process;
    QCOMPARE(startInterruptBytesChild(process, events, directory.path()), QString());
    QVERIFY(process.cancelPendingWritesAndInterrupt());
    QVERIFY2(events.waitForOutput("span_interrupt_bytes=03", std::chrono::seconds(8)), events.diagnostic().constData());
    QVERIFY(events.waitForExit(std::chrono::seconds(8)));
    QCOMPARE(events.exitCode, quint32(0));
    process.stop();
}

void WinConPtyProcessTest::interruptClosesDeliveredPaste()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    ProcessEvents events;
    WinConPtyProcess process;
    QCOMPARE(startInterruptBytesChild(process, events, directory.path()), QString());
    QVERIFY(process.write(QByteArrayLiteral("\x1b[200~"), span::terminal::TerminalInputKind::PasteBegin));
    QVERIFY2(events.waitForOutput("span_paste_begin_received", std::chrono::seconds(8)),
             events.diagnostic().constData());
    QVERIFY(process.cancelPendingWritesAndInterrupt());
    // The delivered opener is closed before the interrupt.
    QVERIFY2(events.waitForOutput("span_interrupt_bytes=1b5b3230307e1b5b3230317e03", std::chrono::seconds(8)),
             events.diagnostic().constData());
    QVERIFY(events.waitForExit(std::chrono::seconds(8)));
    QCOMPARE(events.exitCode, quint32(0));
    process.stop();
}

void WinConPtyProcessTest::stopDoesNotHang() {
    const QString cmd = QStandardPaths::findExecutable(QStringLiteral("cmd.exe"));
    QVERIFY(!cmd.isEmpty());

    ProcessEvents events;
    WinConPtyProcess process;
    WinConPtyProcess::StartOptions options;
    options.commandLine = quoteCommand(cmd) + QStringLiteral(" /d /q");

    QString error;
    QVERIFY2(process.start(options,
                           [&](const QByteArray &bytes, bool) { events.append(bytes); },
                           [&](quint32 code) { events.finish(code); },
                           &error),
             qPrintable(error));
    QVERIFY(process.write(QByteArrayLiteral("ping -t 127.0.0.1 > nul\r")));
    QTest::qWait(150);

    QElapsedTimer timer;
    timer.start();
    process.stop();
    QVERIFY2(timer.elapsed() < 3000, "Stopping ConPTY took too long.");
}

namespace {
DWORD parentProcessId(DWORD processId)
{
    const HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) {
        return 0;
    }
    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    DWORD parent = 0;
    for (BOOL found = Process32FirstW(snapshot, &entry); found; found = Process32NextW(snapshot, &entry)) {
        if (entry.th32ProcessID == processId) {
            parent = entry.th32ParentProcessID;
            break;
        }
    }
    CloseHandle(snapshot);
    return parent;
}

QString processImageName(DWORD processId)
{
    const HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, processId);
    if (!process) {
        return {};
    }
    wchar_t path[MAX_PATH] = {};
    DWORD size = MAX_PATH;
    QString name;
    if (QueryFullProcessImageNameW(process, 0, path, &size)) {
        name = QFileInfo(QString::fromWCharArray(path)).fileName();
    }
    CloseHandle(process);
    return name;
}

bool jobContainsImage(const QList<quint32> &ids, const QString &image)
{
    for (quint32 id : ids) {
        if (processImageName(DWORD(id)).compare(image, Qt::CaseInsensitive) == 0) {
            return true;
        }
    }
    return false;
}
}

void WinConPtyProcessTest::sessionJobOwnsConsoleHostAndForegroundChild() {
    const QString cmd = QStandardPaths::findExecutable(QStringLiteral("cmd.exe"));
    QVERIFY(!cmd.isEmpty());

    ProcessEvents events;
    WinConPtyProcess process;
    WinConPtyProcess::StartOptions options;
    options.commandLine = quoteCommand(cmd) + QStringLiteral(" /d /q");

    QString error;
    QVERIFY2(process.start(options,
                           [&](const QByteArray &bytes, bool) { events.append(bytes); },
                           [&](quint32 code) { events.finish(code); },
                           &error),
             qPrintable(error));

    const quint32 shellPid = process.processId();
    QVERIFY(shellPid != 0);
    QCOMPARE(parentProcessId(DWORD(shellPid)), GetCurrentProcessId());
    QVERIFY(process.jobProcessIds().contains(shellPid));
    // The build tree carries the bundled conpty.dll, which starts the
    // OpenConsole.exe next to it instead of the system conhost.exe.
    const QString consoleHost = WinConPtyProcess::usesBundledConPty() ? QStringLiteral("OpenConsole.exe")
                                                                       : QStringLiteral("conhost.exe");
    QVERIFY2(jobContainsImage(process.jobProcessIds(), consoleHost),
             "Session job must own the ConPTY console host.");

    QVERIFY(process.write(QByteArrayLiteral("ping -n 8 127.0.0.1\r")));
    QVERIFY2(
        QTest::qWaitFor([&]() { return jobContainsImage(process.jobProcessIds(), QStringLiteral("PING.EXE")); }, 4000),
        "Session job must own the foreground child.");
    process.stop();
}

void WinConPtyProcessTest::buildTreeUsesBundledConPty() {
    // The build copies conpty.dll and OpenConsole.exe next to every
    // executable, so a test run exercises the same console host as a release.
    const QDir dir(QCoreApplication::applicationDirPath());
    QVERIFY(QFileInfo(dir.filePath(QStringLiteral("conpty.dll"))).isFile());
    QVERIFY(QFileInfo(dir.filePath(QStringLiteral("OpenConsole.exe"))).isFile());
    QVERIFY(WinConPtyProcess::usesBundledConPty());
}

QTEST_MAIN(WinConPtyProcessTest)

#include "win_conpty_process_test.moc"
