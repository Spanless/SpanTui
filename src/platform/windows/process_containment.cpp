#include "platform/windows/process_containment.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <QDir>
#include <QFileInfo>

#include <vector>

#ifndef PROC_THREAD_ATTRIBUTE_JOB_LIST
#  define PROC_THREAD_ATTRIBUTE_JOB_LIST \
      ProcThreadAttributeValue(13, FALSE, TRUE, FALSE)
#endif

namespace span::platform::process {
namespace {

HANDLE g_applicationJob = nullptr;
bool g_applicationJobOwnsProcess = false;

QString windowsErrorMessage(const QString &prefix, DWORD error = GetLastError())
{
    wchar_t *buffer = nullptr;
    const DWORD size = FormatMessageW(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr,
        error,
        MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
        reinterpret_cast<LPWSTR>(&buffer),
        0,
        nullptr);
    QString message = prefix;
    if (size > 0 && buffer) {
        message += QStringLiteral(" ") + QString::fromWCharArray(buffer).trimmed();
        LocalFree(buffer);
    } else {
        message += QStringLiteral(" Error %1.").arg(error);
    }
    return message;
}

// CommandLineToArgvW-compatible quoting so paths with spaces stay one argument.
QString quoteArgument(const QString &argument)
{
    if (argument.isEmpty()) {
        return QStringLiteral("\"\"");
    }
    const bool needsQuotes = argument.contains(QLatin1Char(' '))
        || argument.contains(QLatin1Char('\t'))
        || argument.contains(QLatin1Char('"'))
        || argument.contains(QLatin1Char('\n'))
        || argument.contains(QLatin1Char('\v'));
    if (!needsQuotes) {
        return argument;
    }

    QString quoted;
    quoted.reserve(argument.size() + 2);
    quoted.append(QLatin1Char('"'));
    int backslashes = 0;
    for (QChar ch : argument) {
        if (ch == QLatin1Char('\\')) {
            ++backslashes;
            continue;
        }
        if (ch == QLatin1Char('"')) {
            quoted.append(QString(backslashes * 2 + 1, QLatin1Char('\\')));
            quoted.append(QLatin1Char('"'));
            backslashes = 0;
            continue;
        }
        if (backslashes > 0) {
            quoted.append(QString(backslashes, QLatin1Char('\\')));
            backslashes = 0;
        }
        quoted.append(ch);
    }
    if (backslashes > 0) {
        quoted.append(QString(backslashes * 2, QLatin1Char('\\')));
    }
    quoted.append(QLatin1Char('"'));
    return quoted;
}

QString joinCommandLine(const QString &executable, const QStringList &arguments)
{
    QString command = quoteArgument(QDir::toNativeSeparators(executable));
    for (const QString &argument : arguments) {
        command += QLatin1Char(' ');
        command += quoteArgument(argument);
    }
    return command;
}

class StartupAttributes final
{
public:
    explicit StartupAttributes(HANDLE job)
    {
        m_info.StartupInfo.cb = sizeof(STARTUPINFOW);
        if (!job) {
            return;
        }
        SIZE_T bytes = 0;
        InitializeProcThreadAttributeList(nullptr, 1, 0, &bytes);
        m_info.lpAttributeList = static_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(
            HeapAlloc(GetProcessHeap(), 0, bytes));
        if (!m_info.lpAttributeList) {
            return;
        }
        if (!InitializeProcThreadAttributeList(m_info.lpAttributeList, 1, 0, &bytes)) {
            HeapFree(GetProcessHeap(), 0, m_info.lpAttributeList);
            m_info.lpAttributeList = nullptr;
            return;
        }
        m_jobList[0] = job;
        if (!UpdateProcThreadAttribute(m_info.lpAttributeList,
                                       0,
                                       PROC_THREAD_ATTRIBUTE_JOB_LIST,
                                       m_jobList,
                                       sizeof(m_jobList),
                                       nullptr,
                                       nullptr)) {
            DeleteProcThreadAttributeList(m_info.lpAttributeList);
            HeapFree(GetProcessHeap(), 0, m_info.lpAttributeList);
            m_info.lpAttributeList = nullptr;
            return;
        }
        m_info.StartupInfo.cb = sizeof(STARTUPINFOEXW);
    }

    ~StartupAttributes()
    {
        if (m_info.lpAttributeList) {
            DeleteProcThreadAttributeList(m_info.lpAttributeList);
            HeapFree(GetProcessHeap(), 0, m_info.lpAttributeList);
        }
    }

    STARTUPINFOEXW *get() { return &m_info; }
    bool hasJobAttribute() const { return m_info.lpAttributeList != nullptr; }

private:
    STARTUPINFOEXW m_info{};
    HANDLE m_jobList[1] = {};
};

bool prepareLaunch(const QString &executable,
                   const QStringList &arguments,
                   const QString &workingDirectory,
                   std::wstring *command,
                   std::wstring *nativeDirectory,
                   QString *errorMessage)
{
    const QFileInfo info(executable);
    if (!info.exists() || !info.isFile()) {
        if (errorMessage) {
            *errorMessage = QStringLiteral("Executable is missing: %1").arg(executable);
        }
        return false;
    }

    QString directory = workingDirectory.trimmed();
    if (!directory.isEmpty() && !QFileInfo(directory).isDir()) {
        if (errorMessage) {
            *errorMessage = QStringLiteral("Working directory is missing or not a directory: %1")
                                .arg(directory);
        }
        return false;
    }
    if (!directory.isEmpty()) {
        directory = QDir::toNativeSeparators(directory);
    }

    *command = joinCommandLine(info.absoluteFilePath(), arguments).toStdWString();
    *nativeDirectory = directory.toStdWString();
    return true;
}

} // namespace

void installApplicationContainment()
{
    if (g_applicationJob) {
        return;
    }

    HANDLE job = CreateJobObjectW(nullptr, nullptr);
    if (!job) {
        return;
    }
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
    // BREAKAWAY_OK is required so the installer can leave this job. Without it,
    // QProcess::startDetached still inherits kill-on-close and dies with us.
    limits.BasicLimitInformation.LimitFlags =
        JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE | JOB_OBJECT_LIMIT_BREAKAWAY_OK;
    if (!SetInformationJobObject(job, JobObjectExtendedLimitInformation, &limits, sizeof(limits))) {
        CloseHandle(job);
        return;
    }
    g_applicationJob = job;
    if (AssignProcessToJobObject(job, GetCurrentProcess())) {
        g_applicationJobOwnsProcess = true;
        return;
    }
    BOOL alreadyInJob = FALSE;
    if (IsProcessInJob(GetCurrentProcess(), job, &alreadyInJob) && alreadyInJob) {
        g_applicationJobOwnsProcess = true;
    }
}

bool applicationJobActive()
{
    return g_applicationJobOwnsProcess;
}

bool applicationJobContainsProcess(quint32 processId)
{
    if (!g_applicationJob || processId == 0) {
        return false;
    }
    const HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, DWORD(processId));
    if (!process) {
        return false;
    }
    BOOL inJob = FALSE;
    const BOOL queried = IsProcessInJob(process, g_applicationJob, &inJob);
    CloseHandle(process);
    return queried && inJob;
}

quint32 startOwnedProcess(const QString &executable,
                          const QStringList &arguments,
                          const QString &workingDirectory,
                          QString *errorMessage)
{
    std::wstring command;
    std::wstring nativeDirectory;
    if (!prepareLaunch(executable, arguments, workingDirectory, &command, &nativeDirectory,
                       errorMessage)) {
        return 0;
    }

    StartupAttributes startup(g_applicationJob);
    PROCESS_INFORMATION processInfo{};
    DWORD flags = CREATE_UNICODE_ENVIRONMENT | CREATE_SUSPENDED;
    if (startup.hasJobAttribute()) {
        flags |= EXTENDED_STARTUPINFO_PRESENT;
    }
    STARTUPINFOW *startupInfo = &startup.get()->StartupInfo;
    const BOOL created = CreateProcessW(
        nullptr,
        command.data(),
        nullptr,
        nullptr,
        FALSE,
        flags,
        nullptr,
        nativeDirectory.empty() ? nullptr : nativeDirectory.c_str(),
        startupInfo,
        &processInfo);
    if (!created) {
        if (errorMessage) {
            *errorMessage = windowsErrorMessage(QStringLiteral("Could not start process:"));
        }
        return 0;
    }

    if (g_applicationJob) {
        BOOL alreadyInJob = FALSE;
        if (!IsProcessInJob(processInfo.hProcess, g_applicationJob, &alreadyInJob)) {
            alreadyInJob = FALSE;
        }
        if (!alreadyInJob && !AssignProcessToJobObject(g_applicationJob, processInfo.hProcess)) {
            TerminateProcess(processInfo.hProcess, 1);
            CloseHandle(processInfo.hThread);
            CloseHandle(processInfo.hProcess);
            if (errorMessage) {
                *errorMessage = windowsErrorMessage(
                    QStringLiteral("Could not assign the process to the application job:"));
            }
            return 0;
        }
    }

    if (ResumeThread(processInfo.hThread) == static_cast<DWORD>(-1)) {
        TerminateProcess(processInfo.hProcess, 1);
        CloseHandle(processInfo.hThread);
        CloseHandle(processInfo.hProcess);
        if (errorMessage) {
            *errorMessage = windowsErrorMessage(QStringLiteral("Could not resume process:"));
        }
        return 0;
    }

    const DWORD processId = processInfo.dwProcessId;
    CloseHandle(processInfo.hThread);
    CloseHandle(processInfo.hProcess);
    return quint32(processId);
}

quint32 startUnaffiliatedProcess(const QString &executable,
                                 const QStringList &arguments,
                                 const QString &workingDirectory,
                                 QString *errorMessage)
{
    std::wstring command;
    std::wstring nativeDirectory;
    if (!prepareLaunch(executable, arguments, workingDirectory, &command, &nativeDirectory,
                       errorMessage)) {
        return 0;
    }

    STARTUPINFOW startupInfo{};
    startupInfo.cb = sizeof(startupInfo);
    PROCESS_INFORMATION processInfo{};
    DWORD flags = CREATE_UNICODE_ENVIRONMENT | CREATE_NEW_PROCESS_GROUP;
    // The installer must never inherit a kill-on-close job: the UI exits right
    // after launching it, and a job-affiliated installer would be terminated
    // while replacing files. Request breakaway whenever this process is in
    // any job (our own job allows it; an outer CI/IDE job may not).
    BOOL processInAnyJob = FALSE;
    IsProcessInJob(GetCurrentProcess(), nullptr, &processInAnyJob);
    const bool detachRequested = g_applicationJobOwnsProcess || processInAnyJob;
    if (detachRequested) {
        flags |= CREATE_BREAKAWAY_FROM_JOB;
    }
    const BOOL created = CreateProcessW(
        nullptr,
        command.data(),
        nullptr,
        nullptr,
        FALSE,
        flags,
        nullptr,
        nativeDirectory.empty() ? nullptr : nativeDirectory.c_str(),
        &startupInfo,
        &processInfo);
    if (!created && detachRequested) {
        // Fail closed: silently dropping CREATE_BREAKAWAY_FROM_JOB would let
        // the installer be killed mid-write, which is worse than not updating.
        if (errorMessage) {
            *errorMessage = windowsErrorMessage(
                QStringLiteral("Could not start the process detached from its job:"));
        }
        return 0;
    }
    if (!created) {
        if (errorMessage) {
            *errorMessage = windowsErrorMessage(QStringLiteral("Could not start process:"));
        }
        return 0;
    }

    const DWORD processId = processInfo.dwProcessId;
    CloseHandle(processInfo.hThread);
    CloseHandle(processInfo.hProcess);
    return quint32(processId);
}

} // namespace span::platform::process
