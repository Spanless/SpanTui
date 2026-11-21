#include "platform/crash_handler.h"

#include "core/product_paths.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QString>
#include <QSysInfo>
#include <QTextStream>

#include <atomic>
#include <csignal>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <exception>
#include <mutex>

#include <execinfo.h>
#include <fcntl.h>
#include <mach-o/dyld.h>
#include <pthread.h>
#include <unistd.h>

namespace crash_handler {
namespace {

constexpr int kRingSlots = 64;
constexpr int kRingLineMax = 512;
constexpr int kPathMax = 1024;
constexpr int kStackFrames = 48;
// Room for the handler itself when the fault is a stack overflow.
constexpr size_t kAltStackBytes = 64 * 1024;
constexpr int kFatalSignals[] = {SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGABRT, SIGTRAP};

std::atomic_bool g_installed{false};
std::atomic_bool g_reported{false};

// Pre-allocated for the signal path, which may not allocate or lock.
char g_latestPath[kPathMax] = {};
char g_historyPath[kPathMax] = {};
char g_executablePath[kPathMax] = {};

// Recent Qt log lines — written from the message handler under a mutex; the
// crash path copies them best-effort without locking.
char g_ring[kRingSlots][kRingLineMax];
std::atomic<unsigned> g_ringWrite{0};
std::mutex g_ringMutex;

QString g_latestPathDisplay;

void copyCString(char *dst, size_t dstSize, const char *src)
{
    if (!dst || dstSize == 0) return;
    std::snprintf(dst, dstSize, "%s", src ? src : "");
}

void writeCString(int fd, const char *text)
{
    if (fd < 0 || !text) return;
    size_t left = std::strlen(text);
    while (left > 0) {
        const ssize_t written = ::write(fd, text, left);
        if (written <= 0) return;
        text += written;
        left -= size_t(written);
    }
}

// vsnprintf into a stack buffer: not formally async-signal-safe, but it does
// not allocate for these plain formats, which is what a crash report needs.
void writeFmt(int fd, const char *fmt, ...)
{
    char buffer[1024];
    va_list args;
    va_start(args, fmt);
    std::vsnprintf(buffer, sizeof(buffer), fmt, args);
    va_end(args);
    writeCString(fd, buffer);
}

void writeRing(int fd)
{
    writeCString(fd, "Recent log lines:\n");
    const unsigned writeIdx = g_ringWrite.load(std::memory_order_relaxed);
    const unsigned start = writeIdx > kRingSlots ? writeIdx - kRingSlots : 0;
    for (unsigned i = start; i < writeIdx; ++i) {
        const char *line = g_ring[i % kRingSlots];
        if (!line[0]) continue;
        writeCString(fd, "  ");
        writeCString(fd, line);
        writeCString(fd, "\n");
    }
}

void writeReport(int fd, const char *reason, const siginfo_t *info)
{
    if (fd < 0) return;
    const time_t now = std::time(nullptr);
    struct tm local{};
    localtime_r(&now, &local);
    char stamp[32];
    std::strftime(stamp, sizeof(stamp), "%Y-%m-%d %H:%M:%S", &local);
    uint64_t threadId = 0;
    pthread_threadid_np(nullptr, &threadId);
    writeFmt(fd, "===== SpanTui crash %s =====\nReason: %s\nPID: %d  TID: %llu\n", stamp, reason ? reason : "unknown",
             int(getpid()), static_cast<unsigned long long>(threadId));
    if (info) {
        writeFmt(fd, "Signal: %d  code: %d  address: %p\n", info->si_signo, info->si_code, info->si_addr);
    }
    if (g_executablePath[0]) writeFmt(fd, "Executable: %s\n", g_executablePath);
    void *frames[kStackFrames] = {};
    const int count = backtrace(frames, kStackFrames);
    writeCString(fd, "Stack (backtrace):\n");
    backtrace_symbols_fd(frames, count, fd);
    writeRing(fd);
    writeCString(fd, "===== end =====\n\n");
    fsync(fd);
}

void report(const char *reason, const siginfo_t *info)
{
    bool expected = false;
    if (!g_reported.compare_exchange_strong(expected, true)) return;
    const int latest = g_latestPath[0] ? ::open(g_latestPath, O_WRONLY | O_CREAT | O_TRUNC, 0600) : -1;
    const int history = g_historyPath[0] ? ::open(g_historyPath, O_WRONLY | O_CREAT | O_APPEND, 0600) : -1;
    writeReport(latest, reason, info);
    writeReport(history, reason, info);
    if (latest >= 0) ::close(latest);
    if (history >= 0) ::close(history);
    writeCString(STDERR_FILENO, "SpanTui crash recorded: ");
    writeCString(STDERR_FILENO, g_latestPath);
    writeCString(STDERR_FILENO, "\n");
}

void terminateHandler()
{
    report("std::terminate", nullptr);
    std::abort();
}

void signalHandler(int sig, siginfo_t *info, void *)
{
    char reason[64];
    std::snprintf(reason, sizeof(reason), "POSIX signal %d (%s)", sig, sys_signame[sig]);
    report(reason, info);
    // Restore the default action and re-raise so the process still ends the
    // usual way (and the system crash reporter sees it).
    struct sigaction fallback{};
    fallback.sa_handler = SIG_DFL;
    sigemptyset(&fallback.sa_mask);
    sigaction(sig, &fallback, nullptr);
    raise(sig);
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
    report(reason ? reason : "reportFatal", nullptr);
}

QString latestCrashLogPath()
{
    return g_latestPathDisplay;
}

void install(const QString &logDirectory)
{
    if (g_installed.exchange(true)) return;

    QString dir = logDirectory;
    if (dir.isEmpty()) {
        dir = span::core::ProductPaths::crashLogDirectory();
    }
    QDir().mkpath(dir);

    const QString latest = dir + QStringLiteral("/crash-latest.log");
    const QString history = dir + QStringLiteral("/crash.log");
    g_latestPathDisplay = latest;
    copyCString(g_latestPath, sizeof(g_latestPath), QFile::encodeName(latest).constData());
    copyCString(g_historyPath, sizeof(g_historyPath), QFile::encodeName(history).constData());
    uint32_t executableSize = sizeof(g_executablePath);
    if (_NSGetExecutablePath(g_executablePath, &executableSize) != 0) g_executablePath[0] = '\0';

    // Startup breadcrumb so we know the handler armed and where reports go.
    {
        QFile breadcrumb(history);
        if (breadcrumb.open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text)) {
            QTextStream stream(&breadcrumb);
            stream << QDateTime::currentDateTime().toString(Qt::ISODate) << " crash_handler installed"
                   << " exe=" << QCoreApplication::applicationFilePath()
                   << " ver=" << QCoreApplication::applicationVersion() << " os=" << QSysInfo::prettyProductName()
                   << " arch=" << QSysInfo::currentCpuArchitecture() << " latest=" << latest << '\n';
        }
    }

    std::set_terminate(terminateHandler);

    // Allocated once and never freed: it must outlive any fault.
    stack_t altStack{};
    altStack.ss_sp = std::malloc(kAltStackBytes);
    altStack.ss_size = kAltStackBytes;
    if (altStack.ss_sp) sigaltstack(&altStack, nullptr);

    struct sigaction action{};
    action.sa_sigaction = signalHandler;
    action.sa_flags = SA_SIGINFO | SA_ONSTACK;
    sigemptyset(&action.sa_mask);
    for (int sig : kFatalSignals) {
        sigaction(sig, &action, nullptr);
    }
}

} // namespace crash_handler
