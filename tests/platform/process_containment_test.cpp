#include "platform/windows/process_containment.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTest>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <shobjidl.h>
#include <tlhelp32.h>

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

bool hasExplicitAppUserModelId()
{
    PWSTR appId = nullptr;
    const HRESULT result = GetCurrentProcessExplicitAppUserModelID(&appId);
    CoTaskMemFree(appId);
    return SUCCEEDED(result);
}

void terminatePid(quint32 processId)
{
    if (processId == 0) {
        return;
    }
    const HANDLE process = OpenProcess(PROCESS_TERMINATE, FALSE, DWORD(processId));
    if (!process) {
        return;
    }
    TerminateProcess(process, 1);
    CloseHandle(process);
}
}

class ProcessContainmentTest final : public QObject {
    Q_OBJECT

private slots:
    void ownedChildReportsThisProcessAsParent();
    void refusesMissingExecutable();
    void refusesMissingWorkingDirectory();
    void argumentsReachTheChildUnchanged();
    void unstartableImageReportsTheWindowsError();
    void installDoesNotAssignUnregisteredAppUserModelId();
    void applicationJobContainsCurrentProcessAfterInstall();
    void unaffiliatedChildBreaksAwayFromApplicationJob();
    void unaffiliatedStartRefusesMissingExecutable();
    void unaffiliatedStartRefusesMissingWorkingDirectory();
};

void ProcessContainmentTest::ownedChildReportsThisProcessAsParent()
{
    const QString cmd = QStandardPaths::findExecutable(QStringLiteral("cmd.exe"));
    QVERIFY(!cmd.isEmpty());

    QString error;
    const quint32 pid = span::platform::process::startOwnedProcess(
        cmd,
        {QStringLiteral("/d"), QStringLiteral("/c"), QStringLiteral("ping"),
         QStringLiteral("-n"), QStringLiteral("6"), QStringLiteral("127.0.0.1")},
        QString(),
        &error);
    QVERIFY2(pid != 0, qPrintable(error));
    QCOMPARE(parentProcessId(DWORD(pid)), GetCurrentProcessId());
    terminatePid(pid);
}

void ProcessContainmentTest::refusesMissingExecutable()
{
    QString error;
    QCOMPARE(span::platform::process::startOwnedProcess(
                 QStringLiteral("Z:/missing-span-child.exe"),
                 {},
                 QString(),
                 &error),
             quint32(0));
    QVERIFY(error.contains(QStringLiteral("missing")));
}

void ProcessContainmentTest::refusesMissingWorkingDirectory()
{
    const QString cmd = QStandardPaths::findExecutable(QStringLiteral("cmd.exe"));
    QVERIFY(!cmd.isEmpty());
    QString error;
    QCOMPARE(span::platform::process::startOwnedProcess(
                 cmd,
                 {QStringLiteral("/d"), QStringLiteral("/c"), QStringLiteral("exit")},
                 QStringLiteral("Z:/missing-span-cwd"),
                 &error),
             quint32(0));
    QVERIFY(error.contains(QStringLiteral("Working directory")));
}

void ProcessContainmentTest::argumentsReachTheChildUnchanged()
{
    // The child sees exactly the list it was given, whatever needs quoting —
    // the executable path itself included.
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString child = dir.filePath(QStringLiteral("with space/argv_echo_child.exe"));
    QVERIFY(QDir().mkpath(QFileInfo(child).absolutePath()));
    QVERIFY(QFile::copy(
        QDir(QCoreApplication::applicationDirPath()).filePath(QStringLiteral("argv_echo_child.exe")),
        child));
    const QStringList arguments{
        QStringLiteral("plain"),          QStringLiteral("two words"),
        QStringLiteral("quote\"inside"),  QStringLiteral("dir with space\\"),
        QStringLiteral("back\\slash"),    QString(),
        QStringLiteral("tab\there"),      QStringLiteral("测试 路径"),
    };

    QString error;
    const quint32 pid =
        span::platform::process::startOwnedProcess(child, arguments, dir.path(), &error);
    QVERIFY2(pid != 0, qPrintable(error));
    const QString recorded = dir.filePath(QStringLiteral("argv.txt"));
    QTRY_VERIFY_WITH_TIMEOUT(QFileInfo::exists(recorded), 10000);
    QFile file(recorded);
    QVERIFY(file.open(QIODevice::ReadOnly));
    QCOMPARE(QString::fromUtf8(file.readAll()).split(QLatin1Char('\n')), arguments);
}

void ProcessContainmentTest::unstartableImageReportsTheWindowsError()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString fake = dir.filePath(QStringLiteral("not-a-program.exe"));
    QFile file(fake);
    QVERIFY(file.open(QIODevice::WriteOnly));
    file.write("text, not an executable image");
    file.close();

    QString error;
    QCOMPARE(span::platform::process::startOwnedProcess(fake, {}, QString(), &error),
             quint32(0));
    QVERIFY2(error.startsWith(QStringLiteral("Could not start process: ")), qPrintable(error));
    QVERIFY(error.size() > QStringLiteral("Could not start process: ").size());
}

void ProcessContainmentTest::installDoesNotAssignUnregisteredAppUserModelId()
{
    if (hasExplicitAppUserModelId()) QSKIP("The test host already assigned an explicit AppUserModelID.");

    span::platform::process::installApplicationContainment();

    QVERIFY2(!hasExplicitAppUserModelId(), "Process containment must leave the explicit AppUserModelID unset.");
}

void ProcessContainmentTest::applicationJobContainsCurrentProcessAfterInstall()
{
    span::platform::process::installApplicationContainment();
    if (!span::platform::process::applicationJobActive())
        QSKIP("Host already placed this process in a job that rejects nesting.");

    const QString cmd = QStandardPaths::findExecutable(QStringLiteral("cmd.exe"));
    QVERIFY(!cmd.isEmpty());
    QString error;
    const quint32 pid = span::platform::process::startOwnedProcess(
        cmd,
        {QStringLiteral("/d"), QStringLiteral("/c"), QStringLiteral("ping"),
         QStringLiteral("-n"), QStringLiteral("6"), QStringLiteral("127.0.0.1")},
        QString(),
        &error);
    QVERIFY2(pid != 0, qPrintable(error));
    QCOMPARE(parentProcessId(DWORD(pid)), GetCurrentProcessId());
    QVERIFY2(span::platform::process::applicationJobContainsProcess(pid),
             "Owned child must belong to the application job.");
    terminatePid(pid);
}

void ProcessContainmentTest::unaffiliatedChildBreaksAwayFromApplicationJob()
{
    span::platform::process::installApplicationContainment();
    if (!span::platform::process::applicationJobActive())
        QSKIP("Host already placed this process in a job that rejects nesting.");

    const QString cmd = QStandardPaths::findExecutable(QStringLiteral("cmd.exe"));
    QVERIFY(!cmd.isEmpty());
    QString error;
    const quint32 pid = span::platform::process::startUnaffiliatedProcess(
        cmd,
        {QStringLiteral("/d"), QStringLiteral("/c"), QStringLiteral("ping"),
         QStringLiteral("-n"), QStringLiteral("6"), QStringLiteral("127.0.0.1")},
        QString(),
        &error);
    QVERIFY2(pid != 0, qPrintable(error));
    QCOMPARE(parentProcessId(DWORD(pid)), GetCurrentProcessId());
    QVERIFY2(!span::platform::process::applicationJobContainsProcess(pid),
             "Unaffiliated child must leave the application job.");
    terminatePid(pid);
}

void ProcessContainmentTest::unaffiliatedStartRefusesMissingExecutable()
{
    QString error;
    QCOMPARE(span::platform::process::startUnaffiliatedProcess(
                 QStringLiteral("Z:/missing-span-child.exe"),
                 {},
                 QString(),
                 &error),
             quint32(0));
    QVERIFY(error.contains(QStringLiteral("missing")));
}

void ProcessContainmentTest::unaffiliatedStartRefusesMissingWorkingDirectory()
{
    const QString cmd = QStandardPaths::findExecutable(QStringLiteral("cmd.exe"));
    QVERIFY(!cmd.isEmpty());
    QString error;
    QCOMPARE(span::platform::process::startUnaffiliatedProcess(
                 cmd,
                 {QStringLiteral("/d"), QStringLiteral("/c"), QStringLiteral("exit")},
                 QStringLiteral("Z:/missing-span-cwd"),
                 &error),
             quint32(0));
    QVERIFY(error.contains(QStringLiteral("Working directory")));
}

QTEST_MAIN(ProcessContainmentTest)

#include "process_containment_test.moc"
