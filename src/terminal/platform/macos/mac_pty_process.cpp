#include "terminal/platform/macos/mac_pty_process.h"

#include "core/core_constants.h"
#include "terminal/api/platform_terminal.h"
#include "terminal/api/shell_catalog.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QLocale>
#include <QProcess>
#include <QProcessEnvironment>
#include <QStandardPaths>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <climits>
#include <condition_variable>
#include <csignal>
#include <cstring>
#include <deque>
#include <mutex>
#include <thread>
#include <vector>

#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>
#include <langinfo.h>
#include <util.h>
#include <xlocale.h>

namespace span::terminal {

namespace {

constexpr qsizetype kMaximumPendingWriteBytes = 1024 * 1024;
constexpr qsizetype kReadChunkBytes = 64 * 1024;
// How long stop() lets the shell act on SIGHUP before SIGKILL.
constexpr auto kHangupGrace = std::chrono::milliseconds(250);
// Every macOS install has this terminfo entry; xterm-kitty, which Windows
// sessions announce, is not installed.
constexpr QLatin1StringView kTerm{"xterm-256color"};
// The identity of a terminal SpanTui itself was started from, which is not
// the one the shell runs in.
constexpr QLatin1StringView kForeignTerminalVariables[]{
    QLatin1StringView{"TERM_SESSION_ID"},
    QLatin1StringView{"ITERM_SESSION_ID"},
    QLatin1StringView{"LC_TERMINAL"},
    QLatin1StringView{"LC_TERMINAL_VERSION"},
};

QString loginShell()
{
    const QString shell = qEnvironmentVariable("SHELL");
    return !shell.isEmpty() && QFileInfo(shell).isExecutable() ? shell : QStringLiteral("/bin/zsh");
}

QString systemErrorMessage(const QString &prefix, int error)
{
    return prefix + QLatin1Char(' ') + QString::fromLocal8Bit(std::strerror(error));
}

// True when the C library knows `name` as a UTF-8 character locale.
bool isUtf8Locale(const QString &name)
{
    if (name.isEmpty()) {
        return false;
    }
    const locale_t locale = newlocale(LC_CTYPE_MASK, name.toLocal8Bit().constData(), nullptr);
    if (!locale) {
        return false;
    }
    const bool utf8 = qstrcmp(nl_langinfo_l(CODESET, locale), "UTF-8") == 0;
    freelocale(locale);
    return utf8;
}

// The system language as a UTF-8 locale the C library has, else en_US.
QString fallbackUtf8Locale()
{
    const QString system = QLocale::system().name() + QStringLiteral(".UTF-8");
    return isUtf8Locale(system) ? system : QStringLiteral("en_US.UTF-8");
}

// The terminal speaks UTF-8. A locale variable the shell cannot use (unset
// when started from Finder, or a region-language mix such as zh_US.UTF-8
// that macOS has no locale for) leaves it in the C locale: it shows
// non-ASCII input as escaped bytes and erases it a byte at a time.
void ensureUtf8Locale(QProcessEnvironment &environment)
{
    const QString fallback = fallbackUtf8Locale();
    for (const QString &name : {QStringLiteral("LC_ALL"), QStringLiteral("LC_CTYPE")}) {
        const QString value = environment.value(name);
        if (!value.isEmpty() && !isUtf8Locale(value)) {
            environment.insert(name, fallback);
        }
    }
    if (!isUtf8Locale(environment.value(QStringLiteral("LANG")))) {
        environment.insert(QStringLiteral("LANG"), fallback);
    }
}

// Inherited block plus the terminal identity, then the caller's overrides.
std::vector<QByteArray> terminalEnvironment(const QString &colorFgBg, const QHash<QString, QString> &overrides)
{
    QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
    for (const QLatin1StringView name : kForeignTerminalVariables) {
        environment.remove(QString(name));
    }
    environment.insert(QStringLiteral("TERM"), QString(kTerm));
    environment.insert(QStringLiteral("COLORTERM"), QStringLiteral("truecolor"));
    environment.insert(QStringLiteral("TERM_PROGRAM"), QString(core::kProductName));
    environment.insert(QStringLiteral("TERM_PROGRAM_VERSION"), QStringLiteral(SPANTUI_VERSION));
    environment.insert(QStringLiteral("COLORFGBG"), colorFgBg.isEmpty() ? QStringLiteral("15;0") : colorFgBg);
    ensureUtf8Locale(environment);
    for (auto it = overrides.cbegin(); it != overrides.cend(); ++it) {
        environment.insert(it.key(), it.value());
    }
    std::vector<QByteArray> entries;
    for (const QString &entry : environment.toStringList()) {
        entries.push_back(entry.toUtf8());
    }
    return entries;
}

std::vector<char *> pointers(std::vector<QByteArray> &values)
{
    std::vector<char *> result;
    result.reserve(values.size() + 1);
    for (QByteArray &value : values) {
        result.push_back(value.data());
    }
    result.push_back(nullptr);
    return result;
}

quint32 exitCodeOf(int status)
{
    if (WIFEXITED(status)) return quint32(WEXITSTATUS(status));
    // The shell convention for a process ended by a signal.
    if (WIFSIGNALED(status)) return quint32(128 + WTERMSIG(status));
    return 0;
}

// Runs in the forked child, so only async-signal-safe calls: the parent has
// other threads, whose locks may be held forever here.
[[noreturn]] void execChild(const char *program, char *const argv[], char *const envp[], const char *directory,
                            int errorPipe)
{
    // Ignored signals and the blocked mask survive exec.
    sigset_t none;
    sigemptyset(&none);
    sigprocmask(SIG_SETMASK, &none, nullptr);
    struct sigaction defaultAction{};
    defaultAction.sa_handler = SIG_DFL;
    for (int signal = 1; signal < NSIG; ++signal) {
        sigaction(signal, &defaultAction, nullptr);
    }
    // Line editing (erase, kill) counts UTF-8 characters, not bytes.
    struct termios attributes{};
    if (tcgetattr(STDIN_FILENO, &attributes) == 0) {
        attributes.c_iflag |= IUTF8;
        tcsetattr(STDIN_FILENO, TCSANOW, &attributes);
    }
    if (chdir(directory) == 0) {
        execve(program, argv, envp);
    }
    const int error = errno;
    (void)!::write(errorPipe, &error, sizeof error);
    _exit(127);
}

void setDescriptorFlags(int fd, bool nonBlocking)
{
    fcntl(fd, F_SETFD, fcntl(fd, F_GETFD) | FD_CLOEXEC);
    if (nonBlocking) fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
}

} // namespace

// Two threads per session. The waiter blocks in waitpid. The I/O thread
// waits in select() (macOS poll() does not support devices) on the master
// and a wake pipe, reads output, writes queued input and reports the exit
// after the last output. Callbacks arrive on the I/O thread.
struct MacPtyProcess::Impl final {
    int master = -1;
    pid_t pid = 0;
    int wakeRead = -1;
    int wakeWrite = -1;
    OutputCallback outputCallback;
    ExitCallback exitCallback;
    std::atomic_bool running = false;
    std::thread io;
    std::thread waiter;

    // Guards everything below, and writes to the master.
    std::mutex mutex;
    std::condition_variable exitedChanged;
    bool stopping = false;
    bool exited = false;
    int exitStatus = 0;
    // The child exited or the pty closed: output still drains.
    bool writesClosed = false;
    struct PendingWrite {
        QByteArray bytes;
        TerminalInputKind kind;
    };
    std::deque<PendingWrite> pendingWrites;
    QByteArray writeInFlight;
    qsizetype writeOffset = 0;
    bool bracketedPasteInFlight = false;
    qsizetype pendingWriteBytes = 0;

    // Orders resizes against closing the master.
    std::mutex masterMutex;

    ~Impl()
    {
        for (int fd : {master, wakeRead, wakeWrite}) {
            if (fd >= 0) ::close(fd);
        }
    }

    void wake()
    {
        const char byte = 0;
        // A full pipe already holds a wake-up.
        (void)!::write(wakeWrite, &byte, 1);
    }

    void waitForExit()
    {
        int status = 0;
        pid_t result = 0;
        do {
            result = waitpid(pid, &status, 0);
        } while (result < 0 && errno == EINTR);
        {
            const std::lock_guard<std::mutex> lock(mutex);
            exited = true;
            exitStatus = result == pid ? status : 0;
            writesClosed = true;
        }
        exitedChanged.notify_all();
        wake();
    }

    // mutex held. Moves queued input into flight and writes what the pty
    // takes without blocking.
    void pumpWrites()
    {
        while (!writesClosed) {
            if (writeOffset >= writeInFlight.size()) {
                writeInFlight.clear();
                writeOffset = 0;
                if (pendingWrites.empty()) return;
                PendingWrite next = std::move(pendingWrites.front());
                pendingWrites.pop_front();
                if (next.kind == TerminalInputKind::PasteBegin) bracketedPasteInFlight = true;
                if (next.kind == TerminalInputKind::PasteEnd) bracketedPasteInFlight = false;
                pendingWriteBytes -= next.bytes.size();
                writeInFlight = std::move(next.bytes);
            }
            const ssize_t written =
                ::write(master, writeInFlight.constData() + writeOffset, size_t(writeInFlight.size() - writeOffset));
            if (written > 0) {
                writeOffset += written;
            } else if (written < 0 && errno == EINTR) {
                continue;
            } else if (written < 0 && errno == EAGAIN) {
                return;
            } else {
                writesClosed = true;
            }
        }
        writeInFlight.clear();
        writeOffset = 0;
    }

    // Reads one chunk; false once every slave descriptor is closed.
    bool readOutput(std::vector<char> &buffer, bool *more)
    {
        qsizetype filled = 0;
        bool open = true;
        bool dry = false;
        while (filled < kReadChunkBytes) {
            const ssize_t count = ::read(master, buffer.data() + filled, size_t(kReadChunkBytes - filled));
            if (count > 0) {
                filled += count;
            } else if (count < 0 && errno == EINTR) {
                continue;
            } else {
                // EAGAIN: the pty ran dry. 0 or EIO: it closed.
                open = count < 0 && errno == EAGAIN;
                dry = true;
                break;
            }
        }
        if (!dry) {
            int available = 0;
            dry = ioctl(master, FIONREAD, &available) != 0 || available == 0;
        }
        // The chunk ends where the pty ran dry: the end of one producer write.
        if (filled > 0 && outputCallback) outputCallback(QByteArray(buffer.data(), int(filled)), dry);
        *more = open && !dry;
        return open;
    }

    void runIo()
    {
        std::vector<char> buffer(static_cast<size_t>(kReadChunkBytes));
        bool readOpen = true;
        for (;;) {
            bool wantWrite = false;
            bool childExited = false;
            {
                const std::lock_guard<std::mutex> lock(mutex);
                if (stopping) return;
                pumpWrites();
                wantWrite = !writesClosed && writeOffset < writeInFlight.size();
                childExited = exited;
            }
            if (childExited) {
                bool more = readOpen;
                while (more) readOutput(buffer, &more);
                running = false;
                int status = 0;
                {
                    const std::lock_guard<std::mutex> lock(mutex);
                    if (stopping) return;
                    status = exitStatus;
                }
                if (exitCallback) exitCallback(exitCodeOf(status));
                return;
            }

            fd_set readable;
            fd_set writable;
            FD_ZERO(&readable);
            FD_ZERO(&writable);
            FD_SET(wakeRead, &readable);
            if (readOpen) FD_SET(master, &readable);
            if (wantWrite) FD_SET(master, &writable);
            if (select(std::max(master, wakeRead) + 1, &readable, &writable, nullptr, nullptr) < 0) {
                if (errno == EINTR) continue;
                qWarning("Terminal I/O wait failed: %s", std::strerror(errno));
                endAfterIoFailure();
                return;
            }
            if (FD_ISSET(wakeRead, &readable)) {
                char drain[64];
                while (::read(wakeRead, drain, sizeof drain) > 0) {}
            }
            if (readOpen && FD_ISSET(master, &readable)) {
                bool more = true;
                while (more && readOpen) readOpen = readOutput(buffer, &more);
                if (!readOpen) {
                    const std::lock_guard<std::mutex> lock(mutex);
                    writesClosed = true;
                }
            }
        }
    }

    // The I/O thread can no longer serve the pty: the child is ended and its
    // exit reported, so the session does not stay running without I/O.
    void endAfterIoFailure()
    {
        const pid_t foreground = foregroundGroup();
        std::unique_lock<std::mutex> lock(mutex);
        writesClosed = true;
        if (!exited) {
            lock.unlock();
            hangUp(foreground);
            lock.lock();
            if (!exitedChanged.wait_for(lock, kHangupGrace, [this]() { return exited || stopping; })) {
                lock.unlock();
                kill(-pid, SIGKILL);
                kill(pid, SIGKILL);
                lock.lock();
            }
        }
        exitedChanged.wait(lock, [this]() { return exited || stopping; });
        if (stopping) return;
        const int status = exitStatus;
        lock.unlock();
        running = false;
        if (exitCallback) exitCallback(exitCodeOf(status));
    }

    // The process group in the pty's foreground (a job the shell started);
    // -1 when unknown.
    pid_t foregroundGroup()
    {
        const std::lock_guard<std::mutex> lock(masterMutex);
        return master >= 0 ? tcgetpgrp(master) : -1;
    }

    // What closing a terminal window does: SIGHUP to the shell's session and
    // to the job in the foreground, which a job-control shell keeps in a
    // process group of its own.
    void hangUp(pid_t foreground)
    {
        kill(-pid, SIGHUP);
        kill(pid, SIGHUP);
        if (foreground > 0 && foreground != pid) kill(-foreground, SIGHUP);
    }

    // A thread may end its own session from a callback; it cannot join
    // itself.
    static void finish(std::thread &thread)
    {
        if (!thread.joinable()) return;
        if (thread.get_id() == std::this_thread::get_id()) {
            thread.detach();
        } else {
            thread.join();
        }
    }

    void shutdown()
    {
        {
            const std::lock_guard<std::mutex> lock(mutex);
            stopping = true;
            pendingWrites.clear();
            pendingWriteBytes = 0;
            writeInFlight.clear();
            writeOffset = 0;
        }
        exitedChanged.notify_all();
        if (wakeWrite >= 0) wake();
        finish(io);

        bool reaped = false;
        {
            const std::lock_guard<std::mutex> lock(mutex);
            reaped = exited;
        }
        const pid_t foreground = pid > 0 ? foregroundGroup() : -1;
        if (!reaped && pid > 0) hangUp(foreground);
        {
            const std::lock_guard<std::mutex> lock(masterMutex);
            if (master >= 0) ::close(master);
            master = -1;
        }
        if (pid > 0) {
            std::unique_lock<std::mutex> lock(mutex);
            if (!exitedChanged.wait_for(lock, kHangupGrace, [this]() { return exited; })) {
                lock.unlock();
                kill(-pid, SIGKILL);
                kill(pid, SIGKILL);
            }
        }
        // A foreground job that ignores the hangup outlives the shell; it is
        // still this session's, as a job object's processes are on Windows.
        if (foreground > 0 && foreground != pid) kill(-foreground, SIGKILL);
        finish(waiter);
        running = false;
    }
};

MacPtyProcess::MacPtyProcess() = default;

MacPtyProcess::~MacPtyProcess()
{
    stop();
}

QString MacPtyProcess::defaultShellCommand()
{
    return shellCommandLine(loginShell(), QStringLiteral("-l"));
}

bool MacPtyProcess::start(const StartOptions &options, OutputCallback outputCallback, ExitCallback exitCallback,
                          QString *errorMessage)
{
    stop();
    const auto fail = [errorMessage](const QString &message) {
        if (errorMessage) *errorMessage = message;
        return false;
    };

    QString commandLine = options.commandLine.trimmed();
    if (commandLine.isEmpty()) commandLine = defaultShellCommand();
    const QStringList arguments = QProcess::splitCommand(commandLine);
    if (arguments.isEmpty()) return fail(QStringLiteral("Could not start shell: empty command line."));
    QString program = arguments.first();
    if (!program.contains(QLatin1Char('/'))) program = QStandardPaths::findExecutable(program);
    if (program.isEmpty() || !QFileInfo(program).isExecutable() || QFileInfo(program).isDir()) {
        return fail(QStringLiteral("Could not start shell: %1 is not an executable.").arg(arguments.first()));
    }

    const QString requestedDirectory = options.workingDirectory.trimmed();
    QString workingDirectory = QDir::homePath();
    if (!requestedDirectory.isEmpty()) {
        if (!QFileInfo(requestedDirectory).isDir()) {
            return fail(QStringLiteral("Working directory is missing or not a directory: %1").arg(requestedDirectory));
        }
        workingDirectory = requestedDirectory;
    }

    // Everything the child needs is built before fork.
    const QByteArray programPath = QFile::encodeName(program);
    const QByteArray directory = QFile::encodeName(workingDirectory);
    std::vector<QByteArray> argumentBytes;
    for (const QString &argument : arguments) {
        argumentBytes.push_back(argument.toUtf8());
    }
    std::vector<QByteArray> environmentBytes = terminalEnvironment(options.colorFgBg, options.environment);
    const std::vector<char *> argv = pointers(argumentBytes);
    const std::vector<char *> envp = pointers(environmentBytes);

    auto impl = std::make_shared<Impl>();
    int wake[2] = {-1, -1};
    int errorPipe[2] = {-1, -1};
    if (pipe(wake) != 0) return fail(systemErrorMessage(QStringLiteral("Could not start terminal I/O:"), errno));
    impl->wakeRead = wake[0];
    impl->wakeWrite = wake[1];
    setDescriptorFlags(impl->wakeRead, true);
    setDescriptorFlags(impl->wakeWrite, true);
    if (pipe(errorPipe) != 0) return fail(systemErrorMessage(QStringLiteral("Could not start shell:"), errno));
    setDescriptorFlags(errorPipe[0], false);
    setDescriptorFlags(errorPipe[1], false);

    struct winsize size{};
    size.ws_col = static_cast<unsigned short>(std::clamp(options.columns, 1, int(USHRT_MAX)));
    size.ws_row = static_cast<unsigned short>(std::clamp(options.rows, 1, int(USHRT_MAX)));
    int master = -1;
    const pid_t pid = forkpty(&master, nullptr, nullptr, &size);
    if (pid == 0) execChild(programPath.constData(), argv.data(), envp.data(), directory.constData(), errorPipe[1]);
    ::close(errorPipe[1]);
    if (pid < 0) {
        const int error = errno;
        ::close(errorPipe[0]);
        return fail(systemErrorMessage(QStringLiteral("Could not create pseudo terminal:"), error));
    }
    impl->master = master;
    impl->pid = pid;
    setDescriptorFlags(master, true);

    // Closed by a successful exec; otherwise carries its errno.
    int childError = 0;
    ssize_t received = 0;
    do {
        received = ::read(errorPipe[0], &childError, sizeof childError);
    } while (received < 0 && errno == EINTR);
    ::close(errorPipe[0]);
    if (received == ssize_t(sizeof childError)) {
        int status = 0;
        while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
        return fail(systemErrorMessage(QStringLiteral("Could not start shell:"), childError));
    }
    if (master >= FD_SETSIZE || impl->wakeRead >= FD_SETSIZE) {
        kill(pid, SIGKILL);
        int status = 0;
        while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
        return fail(QStringLiteral("Could not start terminal I/O: too many open files."));
    }

    impl->outputCallback = std::move(outputCallback);
    impl->exitCallback = std::move(exitCallback);
    impl->running = true;
    impl->waiter = std::thread([impl]() { impl->waitForExit(); });
    impl->io = std::thread([impl]() { impl->runIo(); });
    std::atomic_store(&m_impl, std::move(impl));
    return true;
}

bool MacPtyProcess::write(const QByteArray &bytes, TerminalInputKind kind)
{
    const auto impl = std::atomic_load(&m_impl);
    if (!impl || bytes.isEmpty() || !impl->running) return false;
    {
        const std::lock_guard<std::mutex> lock(impl->mutex);
        if (impl->stopping || impl->writesClosed || bytes.size() > kMaximumPendingWriteBytes
            || impl->pendingWriteBytes + bytes.size() > kMaximumPendingWriteBytes) {
            return false;
        }
        impl->pendingWrites.push_back({bytes, kind});
        impl->pendingWriteBytes += bytes.size();
    }
    impl->wake();
    return true;
}

bool MacPtyProcess::cancelPendingWritesAndInterrupt()
{
    const auto impl = std::atomic_load(&m_impl);
    if (!impl || !impl->running) return false;
    {
        const std::lock_guard<std::mutex> lock(impl->mutex);
        if (impl->stopping || impl->writesClosed) return false;
        QByteArray control;
        if (impl->bracketedPasteInFlight) control.append("\x1b[201~", 6);
        // The line discipline turns it into SIGINT for the foreground job.
        control.append(char(0x03));
        impl->bracketedPasteInFlight = false;
        impl->pendingWrites.erase(
            std::remove_if(impl->pendingWrites.begin(), impl->pendingWrites.end(),
                           [](const Impl::PendingWrite &write) { return write.kind != TerminalInputKind::Protocol; }),
            impl->pendingWrites.end());
        impl->pendingWriteBytes = control.size();
        for (const auto &write : impl->pendingWrites) impl->pendingWriteBytes += write.bytes.size();
        impl->pendingWrites.push_front({std::move(control), TerminalInputKind::Protocol});
    }
    impl->wake();
    return true;
}

void MacPtyProcess::requestResize(int columns, int rows)
{
    const auto impl = std::atomic_load(&m_impl);
    if (!impl || !impl->running) return;
    struct winsize size{};
    size.ws_col = static_cast<unsigned short>(std::clamp(columns, 1, int(USHRT_MAX)));
    size.ws_row = static_cast<unsigned short>(std::clamp(rows, 1, int(USHRT_MAX)));
    const std::lock_guard<std::mutex> lock(impl->masterMutex);
    // Also signals SIGWINCH to the foreground job.
    if (impl->master >= 0 && ioctl(impl->master, TIOCSWINSZ, &size) != 0) {
        qWarning("TIOCSWINSZ(%d x %d) failed: %s", columns, rows, std::strerror(errno));
    }
}

void MacPtyProcess::stop()
{
    if (const auto impl = std::atomic_exchange(&m_impl, std::shared_ptr<Impl>())) impl->shutdown();
}

bool MacPtyProcess::isRunning() const
{
    const auto impl = std::atomic_load(&m_impl);
    return impl && impl->running;
}

qsizetype MacPtyProcess::pendingWriteBytes() const
{
    const auto impl = std::atomic_load(&m_impl);
    if (!impl) return 0;
    const std::lock_guard<std::mutex> lock(impl->mutex);
    return impl->pendingWriteBytes + impl->writeInFlight.size() - impl->writeOffset;
}

quint32 MacPtyProcess::processId() const
{
    const auto impl = std::atomic_load(&m_impl);
    return impl && impl->running ? quint32(impl->pid) : 0;
}

QList<quint32> MacPtyProcess::jobProcessIds() const
{
    return {};
}

std::unique_ptr<TerminalProcess> createPlatformTerminalProcess()
{
    return std::make_unique<MacPtyProcess>();
}

void setPreferSystemConsoleHost(bool) {}

} // namespace span::terminal
