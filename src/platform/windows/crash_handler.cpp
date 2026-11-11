#include "platform/windows/crash_handler.h"
#include "core/product_paths.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QString>
#include <QSysInfo>
#include <QTextStream>

#include <algorithm>
#include <atomic>
#include <cstdarg>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <mutex>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

namespace crash_handler {
namespace {

constexpr int kRingSlots = 64;
constexpr int kRingLineMax = 512;
constexpr int kPathMax = 1024;
constexpr int kStackFrames = 48;

std::atomic_bool g_installed{false};
std::atomic_bool g_reported{false};

// Pre-allocated paths for the exception path (avoid QString / heap there).
char g_latestPathUtf8[kPathMax] = {};
wchar_t g_latestPathW[kPathMax] = {};
wchar_t g_historyPathW[kPathMax] = {};

// Recent Qt log lines — written from the message handler under a mutex; the
// crash path copies them best-effort without locking if a lock is held.
char g_ring[kRingSlots][kRingLineMax];
std::atomic<unsigned> g_ringWrite{0};
std::mutex g_ringMutex;

QString g_latestPathDisplay;

void copyCString(char *dst, size_t dstSize, const char *src)
{
    if (!dst || dstSize == 0)
        return;
    if (!src) {
        dst[0] = '\0';
        return;
    }
    std::snprintf(dst, dstSize, "%s", src);
}

void storePath(const QString &latest, const QString &history)
{
    g_latestPathDisplay = latest;
    const QByteArray latestUtf8 = latest.toUtf8();
    copyCString(g_latestPathUtf8, sizeof(g_latestPathUtf8), latestUtf8.constData());
    const int n1 = latest.toWCharArray(g_latestPathW);
    if (n1 >= 0 && n1 < kPathMax)
        g_latestPathW[n1] = L'\0';
    else
        g_latestPathW[0] = L'\0';
    const int n2 = history.toWCharArray(g_historyPathW);
    if (n2 >= 0 && n2 < kPathMax)
        g_historyPathW[n2] = L'\0';
    else
        g_historyPathW[0] = L'\0';
}

HANDLE openLogFileW(const wchar_t *path, bool append)
{
    if (!path || !path[0])
        return INVALID_HANDLE_VALUE;
    const DWORD disposition = append ? OPEN_ALWAYS : CREATE_ALWAYS;
    HANDLE h = CreateFileW(path, FILE_APPEND_DATA | GENERIC_WRITE, FILE_SHARE_READ,
                           nullptr, disposition, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE)
        return INVALID_HANDLE_VALUE;
    if (append)
        SetFilePointer(h, 0, nullptr, FILE_END);
    return h;
}

void writeRaw(HANDLE h, const char *data, DWORD len)
{
    if (h == INVALID_HANDLE_VALUE || !data || len == 0)
        return;
    DWORD written = 0;
    WriteFile(h, data, len, &written, nullptr);
}

void writeCString(HANDLE h, const char *text)
{
    if (!text)
        return;
    writeRaw(h, text, static_cast<DWORD>(std::strlen(text)));
}

void writeFmt(HANDLE h, const char *fmt, ...)
{
    char buf[1024];
    va_list args;
    va_start(args, fmt);
    const int n = std::vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    if (n > 0)
        writeRaw(h, buf, static_cast<DWORD>(std::min(n, static_cast<int>(sizeof(buf) - 1))));
}

const char *exceptionName(DWORD code)
{
    switch (code) {
    case EXCEPTION_ACCESS_VIOLATION: return "EXCEPTION_ACCESS_VIOLATION";
    case EXCEPTION_ARRAY_BOUNDS_EXCEEDED: return "EXCEPTION_ARRAY_BOUNDS_EXCEEDED";
    case EXCEPTION_BREAKPOINT: return "EXCEPTION_BREAKPOINT";
    case EXCEPTION_DATATYPE_MISALIGNMENT: return "EXCEPTION_DATATYPE_MISALIGNMENT";
    case EXCEPTION_FLT_DENORMAL_OPERAND: return "EXCEPTION_FLT_DENORMAL_OPERAND";
    case EXCEPTION_FLT_DIVIDE_BY_ZERO: return "EXCEPTION_FLT_DIVIDE_BY_ZERO";
    case EXCEPTION_FLT_INEXACT_RESULT: return "EXCEPTION_FLT_INEXACT_RESULT";
    case EXCEPTION_FLT_INVALID_OPERATION: return "EXCEPTION_FLT_INVALID_OPERATION";
    case EXCEPTION_FLT_OVERFLOW: return "EXCEPTION_FLT_OVERFLOW";
    case EXCEPTION_FLT_STACK_CHECK: return "EXCEPTION_FLT_STACK_CHECK";
    case EXCEPTION_FLT_UNDERFLOW: return "EXCEPTION_FLT_UNDERFLOW";
    case EXCEPTION_ILLEGAL_INSTRUCTION: return "EXCEPTION_ILLEGAL_INSTRUCTION";
    case EXCEPTION_IN_PAGE_ERROR: return "EXCEPTION_IN_PAGE_ERROR";
    case EXCEPTION_INT_DIVIDE_BY_ZERO: return "EXCEPTION_INT_DIVIDE_BY_ZERO";
    case EXCEPTION_INT_OVERFLOW: return "EXCEPTION_INT_OVERFLOW";
    case EXCEPTION_INVALID_DISPOSITION: return "EXCEPTION_INVALID_DISPOSITION";
    case EXCEPTION_NONCONTINUABLE_EXCEPTION: return "EXCEPTION_NONCONTINUABLE_EXCEPTION";
    case EXCEPTION_PRIV_INSTRUCTION: return "EXCEPTION_PRIV_INSTRUCTION";
    case EXCEPTION_SINGLE_STEP: return "EXCEPTION_SINGLE_STEP";
    case EXCEPTION_STACK_OVERFLOW: return "EXCEPTION_STACK_OVERFLOW";
    default: return "EXCEPTION_UNKNOWN";
    }
}

void writeStack(HANDLE h)
{
    void *frames[kStackFrames] = {};
    const USHORT count = CaptureStackBackTrace(0, kStackFrames, frames, nullptr);
    writeCString(h, "Stack (CaptureStackBackTrace):\n");
    for (USHORT i = 0; i < count; ++i) {
        HMODULE module = nullptr;
        char modulePath[MAX_PATH] = {};
        const DWORD flags = GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS
                            | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT;
        if (GetModuleHandleExA(flags, static_cast<LPCSTR>(frames[i]), &module) && module) {
            GetModuleFileNameA(module, modulePath, MAX_PATH);
            const auto base = reinterpret_cast<uintptr_t>(module);
            const auto addr = reinterpret_cast<uintptr_t>(frames[i]);
            writeFmt(h, "  #%02u %p  %s + 0x%llx\n", static_cast<unsigned>(i), frames[i],
                     modulePath[0] ? modulePath : "?",
                     static_cast<unsigned long long>(addr >= base ? addr - base : 0));
        } else {
            writeFmt(h, "  #%02u %p\n", static_cast<unsigned>(i), frames[i]);
        }
    }
}

void writeRing(HANDLE h)
{
    writeCString(h, "Recent log lines:\n");
    // Snapshot indices without waiting forever if the mutex is poisoned mid-crash.
    unsigned writeIdx = g_ringWrite.load(std::memory_order_relaxed);
    const unsigned start = writeIdx > kRingSlots ? writeIdx - kRingSlots : 0;
    for (unsigned i = start; i < writeIdx; ++i) {
        const char *line = g_ring[i % kRingSlots];
        if (!line[0])
            continue;
        writeCString(h, "  ");
        writeCString(h, line);
        writeCString(h, "\n");
    }
}

void writeReportToHandles(const char *reason, EXCEPTION_POINTERS *info)
{
    SYSTEMTIME st = {};
    GetLocalTime(&st);

    auto dumpOne = [&](HANDLE h) {
        if (h == INVALID_HANDLE_VALUE)
            return;
        writeFmt(h,
                 "===== SpanTui crash %04u-%02u-%02u %02u:%02u:%02u =====\n"
                 "Reason: %s\n"
                 "PID: %lu  TID: %lu\n",
                 st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond,
                 reason ? reason : "unknown",
                 static_cast<unsigned long>(GetCurrentProcessId()),
                 static_cast<unsigned long>(GetCurrentThreadId()));

        if (info && info->ExceptionRecord) {
            const EXCEPTION_RECORD *rec = info->ExceptionRecord;
            writeFmt(h, "ExceptionCode: 0x%08lX (%s)\nExceptionAddress: %p\n",
                     static_cast<unsigned long>(rec->ExceptionCode),
                     exceptionName(rec->ExceptionCode),
                     rec->ExceptionAddress);
            if (rec->ExceptionCode == EXCEPTION_ACCESS_VIOLATION
                && rec->NumberParameters >= 2) {
                const ULONG_PTR op = rec->ExceptionInformation[0];
                const ULONG_PTR addr = rec->ExceptionInformation[1];
                writeFmt(h, "Access: %s at %p\n",
                         op == 0 ? "read" : op == 1 ? "write" : "execute/dep",
                         reinterpret_cast<void *>(addr));
            }
        }

        char exePath[MAX_PATH] = {};
        if (GetModuleFileNameA(nullptr, exePath, MAX_PATH))
            writeFmt(h, "Executable: %s\n", exePath);

        writeStack(h);
        writeRing(h);
        writeCString(h, "===== end =====\n\n");
        FlushFileBuffers(h);
    };

    HANDLE latest = openLogFileW(g_latestPathW, false);
    HANDLE history = openLogFileW(g_historyPathW, true);
    dumpOne(latest);
    dumpOne(history);
    if (latest != INVALID_HANDLE_VALUE)
        CloseHandle(latest);
    if (history != INVALID_HANDLE_VALUE)
        CloseHandle(history);

    // Also mirror a short line to the launch log path via stderr if present.
    fprintf(stderr, "SpanTui crash recorded: %s\n", g_latestPathUtf8);
    fflush(stderr);
}

void writeReportWin(const char *reason, EXCEPTION_POINTERS *info)
{
    bool expected = false;
    if (!g_reported.compare_exchange_strong(expected, true))
        return;
    writeReportToHandles(reason, info);
}

LONG WINAPI unhandledExceptionFilter(EXCEPTION_POINTERS *info)
{
    writeReportWin("Unhandled SEH exception", info);
    // Let the default handler run as well (WER / debugger).
    return EXCEPTION_CONTINUE_SEARCH;
}

#if defined(_MSC_VER)
void purecallHandler()
{
    writeReportWin("Pure virtual function call", nullptr);
    std::abort();
}

void invalidParameterHandler(const wchar_t *expression, const wchar_t *function,
                             const wchar_t *file, unsigned int line, uintptr_t)
{
    char reason[512];
    // expression/function/file may be null in release CRT.
    std::snprintf(reason, sizeof(reason),
                  "Invalid parameter (file=%ls func=%ls line=%u expr=%ls)",
                  file ? file : L"?", function ? function : L"?", line,
                  expression ? expression : L"?");
    writeReportWin(reason, nullptr);
    std::abort();
}
#endif

void terminateHandler()
{
    writeReportWin("std::terminate", nullptr);
    std::abort();
}

void signalHandler(int sig)
{
    char reason[64];
    std::snprintf(reason, sizeof(reason), "POSIX signal %d", sig);
    writeReportWin(reason, nullptr);
    // Restore default and re-raise so the process still terminates normally.
    std::signal(sig, SIG_DFL);
    std::raise(sig);
}

} // namespace

void noteMessage(QtMsgType type, const QString &line)
{
    Q_UNUSED(type);
    const QByteArray utf8 = line.toUtf8();
    std::lock_guard<std::mutex> lock(g_ringMutex);
    const unsigned idx = g_ringWrite.fetch_add(1, std::memory_order_relaxed) % kRingSlots;
    copyCString(g_ring[idx], kRingLineMax, utf8.constData());
}

void reportFatal(const char *reason)
{
    writeReportWin(reason ? reason : "reportFatal", nullptr);
}

QString latestCrashLogPath()
{
    return g_latestPathDisplay;
}

void install(const QString &logDirectory)
{
    if (g_installed.exchange(true))
        return;

    QString dir = logDirectory;
    if (dir.isEmpty()) {
        dir = span::core::ProductPaths::crashLogDirectory();
    }
    QDir().mkpath(dir);

    const QString latest = dir + QStringLiteral("/crash-latest.log");
    const QString history = dir + QStringLiteral("/crash.log");
    storePath(latest, history);

    // Startup breadcrumb so we know the handler armed and where reports go.
    {
        QFile breadcrumb(history);
        if (breadcrumb.open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text)) {
            QTextStream stream(&breadcrumb);
            stream << QDateTime::currentDateTime().toString(Qt::ISODate)
                   << " crash_handler installed"
                   << " exe=" << QCoreApplication::applicationFilePath()
                   << " ver=" << QCoreApplication::applicationVersion()
                   << " os=" << QSysInfo::prettyProductName()
                   << " arch=" << QSysInfo::currentCpuArchitecture()
                   << " latest=" << latest
                   << '\n';
        }
    }

    std::set_terminate(terminateHandler);
    std::signal(SIGABRT, signalHandler);
    std::signal(SIGSEGV, signalHandler);
#ifdef SIGILL
    std::signal(SIGILL, signalHandler);
#endif
#ifdef SIGFPE
    std::signal(SIGFPE, signalHandler);
#endif

    SetUnhandledExceptionFilter(unhandledExceptionFilter);
#  if defined(_MSC_VER)
    _set_purecall_handler(purecallHandler);
    _set_invalid_parameter_handler(invalidParameterHandler);
    _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
#  endif
}

} // namespace crash_handler
