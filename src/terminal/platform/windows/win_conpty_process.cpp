#include "win_conpty_process.h"

#include "core/core_constants.h"
#include "terminal/api/shell_catalog.h"

#include <QDebug>
#include <QDir>
#include <QFileInfo>
#include <QList>

#include <algorithm>
#include <atomic>
#include <deque>
#include <mutex>
#include <optional>
#include <utility>
#include <vector>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <aclapi.h>
#include <tlhelp32.h>

namespace {
constexpr DWORD kPipeBufferBytes = 128 * 1024;
constexpr DWORD kPipeDefaultTimeoutMs = 30000;

std::atomic_uint32_t g_pipeCounter = 0;

// Restrict named-pipe DACL to the current process user so other same-session
// processes cannot open \\.\pipe\SpanTui-* during the connect window.
class OwnerOnlyPipeSecurity final {
public:
    OwnerOnlyPipeSecurity() {
        HANDLE token = nullptr;
        if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) {
            return;
        }
        DWORD needed = 0;
        GetTokenInformation(token, TokenUser, nullptr, 0, &needed);
        if (needed == 0) {
            CloseHandle(token);
            return;
        }
        m_tokenUser.resize(needed);
        if (!GetTokenInformation(token, TokenUser, m_tokenUser.data(), needed, &needed)) {
            CloseHandle(token);
            m_tokenUser.clear();
            return;
        }
        CloseHandle(token);

        auto *user = reinterpret_cast<TOKEN_USER *>(m_tokenUser.data());
        EXPLICIT_ACCESSW access{};
        access.grfAccessPermissions = GENERIC_READ | GENERIC_WRITE | SYNCHRONIZE;
        access.grfAccessMode = SET_ACCESS;
        access.grfInheritance = NO_INHERITANCE;
        access.Trustee.TrusteeForm = TRUSTEE_IS_SID;
        access.Trustee.TrusteeType = TRUSTEE_IS_USER;
        access.Trustee.ptstrName = reinterpret_cast<LPWSTR>(user->User.Sid);

        if (SetEntriesInAclW(1, &access, nullptr, &m_acl) != ERROR_SUCCESS || !m_acl) {
            m_acl = nullptr;
            return;
        }

        m_sd = static_cast<PSECURITY_DESCRIPTOR>(
            LocalAlloc(LPTR, SECURITY_DESCRIPTOR_MIN_LENGTH));
        if (!m_sd
            || !InitializeSecurityDescriptor(m_sd, SECURITY_DESCRIPTOR_REVISION)
            || !SetSecurityDescriptorDacl(m_sd, TRUE, m_acl, FALSE)) {
            if (m_sd) {
                LocalFree(m_sd);
                m_sd = nullptr;
            }
            LocalFree(m_acl);
            m_acl = nullptr;
            return;
        }

        m_sa.nLength = sizeof(m_sa);
        m_sa.lpSecurityDescriptor = m_sd;
        m_sa.bInheritHandle = FALSE;
        m_ready = true;
    }

    ~OwnerOnlyPipeSecurity() {
        // DACL pointer is owned separately from the descriptor blob.
        if (m_acl) {
            LocalFree(m_acl);
            m_acl = nullptr;
        }
        if (m_sd) {
            LocalFree(m_sd);
            m_sd = nullptr;
        }
    }

    OwnerOnlyPipeSecurity(const OwnerOnlyPipeSecurity &) = delete;
    OwnerOnlyPipeSecurity &operator=(const OwnerOnlyPipeSecurity &) = delete;

    bool ready() const { return m_ready; }
    SECURITY_ATTRIBUTES *get() { return m_ready ? &m_sa : nullptr; }

private:
    std::vector<BYTE> m_tokenUser;
    PACL m_acl = nullptr;
    PSECURITY_DESCRIPTOR m_sd = nullptr;
    SECURITY_ATTRIBUTES m_sa{};
    bool m_ready = false;
};

class UniqueHandle final {
public:
    UniqueHandle() = default;
    explicit UniqueHandle(HANDLE handle) : m_handle(handle) {}
    ~UniqueHandle() { reset(); }

    UniqueHandle(const UniqueHandle &) = delete;
    UniqueHandle &operator=(const UniqueHandle &) = delete;

    UniqueHandle(UniqueHandle &&other) noexcept
        : m_handle(std::exchange(other.m_handle, nullptr)) {}

    UniqueHandle &operator=(UniqueHandle &&other) noexcept {
        if (this != &other) {
            reset(std::exchange(other.m_handle, nullptr));
        }
        return *this;
    }

    HANDLE get() const { return m_handle; }
    explicit operator bool() const { return m_handle && m_handle != INVALID_HANDLE_VALUE; }

    HANDLE release() { return std::exchange(m_handle, nullptr); }

    void reset(HANDLE handle = nullptr) {
        if (*this) {
            CloseHandle(m_handle);
        }
        m_handle = handle;
    }

private:
    HANDLE m_handle = nullptr;
};

// MinGW / older SDKs may lack this ConPTY-adjacent attribute. Process is still
// assigned via AssignProcessToJobObject after CreateProcess when missing.
#ifndef PROC_THREAD_ATTRIBUTE_JOB_LIST
#  define PROC_THREAD_ATTRIBUTE_JOB_LIST \
      ProcThreadAttributeValue(13, FALSE, TRUE, FALSE)
#endif

std::mutex &pseudoConsoleCreateMutex()
{
    static std::mutex mutex;
    return mutex;
}

std::vector<DWORD> childProcessIds(DWORD parentProcessId)
{
    std::vector<DWORD> ids;
    const HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) {
        return ids;
    }
    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    for (BOOL found = Process32FirstW(snapshot, &entry); found; found = Process32NextW(snapshot, &entry)) {
        if (entry.th32ParentProcessID == parentProcessId) {
            ids.push_back(entry.th32ProcessID);
        }
    }
    CloseHandle(snapshot);
    return ids;
}

// Creating a pseudo console starts its console host (conhost.exe, or the
// bundled OpenConsole.exe) as our child but does not put it in the session
// job. Capture the new children and assign them before the shell runs.
void assignNewChildrenToJob(HANDLE job, const std::vector<DWORD> &before)
{
    if (!job) {
        return;
    }
    const std::vector<DWORD> after = childProcessIds(GetCurrentProcessId());
    for (DWORD processId : after) {
        if (std::find(before.begin(), before.end(), processId) != before.end()) {
            continue;
        }
        const HANDLE process = OpenProcess(
            PROCESS_SET_QUOTA | PROCESS_TERMINATE | PROCESS_QUERY_LIMITED_INFORMATION,
            FALSE,
            processId);
        if (!process) {
            continue;
        }
        AssignProcessToJobObject(job, process);
        CloseHandle(process);
    }
}

// One job per session owns the whole shell tree: closing a terminal kills its
// foreground child too, which would otherwise hold the pseudo console open.
class ConPtyChildJob final {
public:
    ConPtyChildJob() { initialize(); }

    bool assign(HANDLE process)
    {
        if (!process || process == INVALID_HANDLE_VALUE)
            return false;
        if (!m_job)
            return false;
        if (AssignProcessToJobObject(m_job, process))
            return true;
        // Already in a nested/compatible job, or OS rejected a second job.
        return false;
    }

    ~ConPtyChildJob()
    {
        if (m_job) {
            CloseHandle(m_job);
            m_job = nullptr;
        }
    }

    ConPtyChildJob(const ConPtyChildJob &) = delete;
    ConPtyChildJob &operator=(const ConPtyChildJob &) = delete;

    HANDLE handle() const { return m_job; }

    QList<quint32> processIds() const
    {
        QList<quint32> ids;
        if (!m_job) {
            return ids;
        }
        std::vector<BYTE> buffer(sizeof(JOBOBJECT_BASIC_PROCESS_ID_LIST)
                                 + sizeof(ULONG_PTR) * 32);
        for (int attempt = 0; attempt < 6; ++attempt) {
            auto *list = reinterpret_cast<JOBOBJECT_BASIC_PROCESS_ID_LIST *>(buffer.data());
            if (QueryInformationJobObject(m_job,
                                          JobObjectBasicProcessIdList,
                                          buffer.data(),
                                          DWORD(buffer.size()),
                                          nullptr)) {
                ids.reserve(int(list->NumberOfProcessIdsInList));
                for (DWORD i = 0; i < list->NumberOfProcessIdsInList; ++i) {
                    ids.append(quint32(list->ProcessIdList[i]));
                }
                return ids;
            }
            if (GetLastError() != ERROR_MORE_DATA) {
                return ids;
            }
            buffer.resize(buffer.size() * 2);
        }
        return ids;
    }

private:
    void initialize()
    {
        m_job = CreateJobObjectW(nullptr, nullptr);
        if (!m_job)
            return;
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
        // Do not set BREAKAWAY_OK: foreground grandchildren stay in this
        // session's ownership domain.
        limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        if (!SetInformationJobObject(m_job, JobObjectExtendedLimitInformation,
                                     &limits, sizeof(limits))) {
            CloseHandle(m_job);
            m_job = nullptr;
        }
    }

    HANDLE m_job = nullptr;
};

class StartupInfo final {
public:
    StartupInfo() {
        m_info.StartupInfo.cb = sizeof(STARTUPINFOEXW);
        m_info.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
        m_info.StartupInfo.hStdInput = nullptr;
        m_info.StartupInfo.hStdOutput = nullptr;
        m_info.StartupInfo.hStdError = nullptr;
    }

    ~StartupInfo() {
        if (m_info.lpAttributeList) {
            DeleteProcThreadAttributeList(m_info.lpAttributeList);
            HeapFree(GetProcessHeap(), 0, m_info.lpAttributeList);
        }
    }

    // `job` may be null.
    bool attachPseudoConsole(HPCON pseudoConsole, HANDLE job, QString *errorMessage);
    STARTUPINFOEXW *get() { return &m_info; }

private:
    STARTUPINFOEXW m_info{};
    // Storage must outlive CreateProcess when JOB_LIST is used.
    HANDLE m_jobList[1] = {};
};

// Only when no command line was given.
QString findShell() {
    const auto shells = span::terminal::detectShells();
    return shells.isEmpty() ? QStringLiteral("cmd.exe")
                            : span::terminal::shellCommandLine(shells.first().executable,
                                               shells.first().arguments);
}

QString windowsErrorMessage(const QString &prefix, DWORD error = GetLastError()) {
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

// Chosen once per process. The bundled conpty.dll + OpenConsole.exe gives
// every Windows build the same console host; the system ConPTY varies with
// the build and is the fallback (bundle missing or damaged, --conpty=system).
struct ConPtyApi {
    using CreateFn = HRESULT(WINAPI *)(COORD, HANDLE, HANDLE, DWORD, HPCON *);
    using ResizeFn = HRESULT(WINAPI *)(HPCON, COORD);
    using CloseFn = void(WINAPI *)(HPCON);
    // ConptyClearPseudoConsole(hPC, keepCursorRow): conhost drops every row
    // but the cursor's, which becomes the top row; it repaints nothing.
    using ClearFn = HRESULT(WINAPI *)(HPCON, BOOL);
    CreateFn create = CreatePseudoConsole;
    ResizeFn resize = ResizePseudoConsole;
    CloseFn close = ClosePseudoConsole;
    ClearFn clear = nullptr;
    bool bundled = false;
};

template <typename Fn>
Fn procedure(HMODULE module, const char *name)
{
    return reinterpret_cast<Fn>(reinterpret_cast<void (*)()>(GetProcAddress(module, name)));
}

QString applicationDirectory()
{
    std::wstring path(MAX_PATH, L'\0');
    for (;;) {
        const DWORD length = GetModuleFileNameW(nullptr, path.data(), DWORD(path.size()));
        if (length == 0) {
            return {};
        }
        if (length < path.size()) {
            path.resize(length);
            break;
        }
        path.resize(path.size() * 2);
    }
    return QFileInfo(QString::fromStdWString(path)).absolutePath();
}

std::atomic_bool g_preferSystemConPty {false};

ConPtyApi loadConPtyApi()
{
    ConPtyApi api;
    if (g_preferSystemConPty.load()) {
        qInfo("ConPTY: system (--conpty=system).");
        return api;
    }
    const QDir directory(applicationDirectory());
    const QString library = QDir::toNativeSeparators(directory.filePath(QStringLiteral("conpty.dll")));
    if (!QFileInfo(library).isFile()) {
        qInfo("ConPTY: system (no bundled conpty.dll).");
        return api;
    }
    // conpty.dll silently starts the system conhost.exe when OpenConsole.exe
    // is missing: report the broken bundle instead of running half of it.
    if (!QFileInfo(directory.filePath(QStringLiteral("OpenConsole.exe"))).isFile()) {
        qWarning().noquote() << "Bundled ConPTY is incomplete: OpenConsole.exe is missing next to"
                             << library << "- using the system ConPTY.";
        return api;
    }
    // Absolute path and restricted search: neither the DLL nor its imports
    // resolve through the current directory, which is the opened folder.
    const HMODULE module = LoadLibraryExW(reinterpret_cast<LPCWSTR>(library.utf16()), nullptr,
                                          LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR
                                              | LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!module) {
        qWarning().noquote() << windowsErrorMessage(
            QStringLiteral("Cannot load the bundled ConPTY %1; using the system ConPTY:").arg(library));
        return api;
    }
    const auto create = procedure<ConPtyApi::CreateFn>(module, "ConptyCreatePseudoConsole");
    const auto resize = procedure<ConPtyApi::ResizeFn>(module, "ConptyResizePseudoConsole");
    const auto close = procedure<ConPtyApi::CloseFn>(module, "ConptyClosePseudoConsole");
    if (!create || !resize || !close) {
        qWarning().noquote() << "The bundled ConPTY" << library
                             << "lacks the Conpty* entry points; using the system ConPTY.";
        FreeLibrary(module);
        return api;
    }
    // Never unloaded: its pseudo consoles may live until the process exits.
    api.create = create;
    api.resize = resize;
    api.close = close;
    api.clear = procedure<ConPtyApi::ClearFn>(module, "ConptyClearPseudoConsole");
    api.bundled = true;
    qInfo().noquote() << "ConPTY: bundled" << library;
    return api;
}

const ConPtyApi &conPtyApi()
{
    static const ConPtyApi api = loadConPtyApi();
    return api;
}

QString pipeName(const QString &direction) {
    return QStringLiteral("\\\\.\\pipe\\SpanTui-%1-%2-%3")
        .arg(GetCurrentProcessId())
        .arg(++g_pipeCounter)
        .arg(direction);
}

bool environmentEntryMatches(const std::wstring &entry, const wchar_t *name) {
    const size_t nameLength = std::wcslen(name);
    return entry.size() > nameLength && entry.at(nameLength) == L'=' &&
           _wcsnicmp(entry.c_str(), name, nameLength) == 0;
}

std::vector<wchar_t> terminalEnvironment(const QString &colorFgBg,
                                         const QHash<QString, QString> &overrides) {
    std::vector<std::wstring> entries;
    if (LPWCH inherited = GetEnvironmentStringsW()) {
        for (const wchar_t *cursor = inherited; *cursor; cursor += std::wcslen(cursor) + 1) {
            entries.emplace_back(cursor);
        }
        FreeEnvironmentStringsW(inherited);
    }

    const auto replace = [&entries](const wchar_t *name, const wchar_t *value) {
        entries.erase(std::remove_if(entries.begin(), entries.end(), [name](const std::wstring &entry) {
                          return environmentEntryMatches(entry, name);
                      }),
                      entries.end());
        entries.emplace_back(std::wstring(name) + L"=" + value);
    };

    // Allow-list TUIs negotiate the Kitty keyboard protocol only for
    // xterm-kitty; TERM_PROGRAM carries the real identity.
    replace(L"TERM", L"xterm-kitty");
    replace(L"COLORTERM", L"truecolor");
    {
        const std::wstring programName = QString(span::core::kProductName).toStdWString();
        replace(L"TERM_PROGRAM", programName.c_str());
        const std::wstring programVersion =
            QStringLiteral(SPANTUI_VERSION).toStdWString();
        replace(L"TERM_PROGRAM_VERSION", programVersion.c_str());
    }
    const std::wstring colorFgBgValue =
        (colorFgBg.isEmpty() ? QStringLiteral("15;0") : colorFgBg).toStdWString();
    replace(L"COLORFGBG", colorFgBgValue.c_str());

    // An inherited WT_SESSION makes TUIs apply Windows Terminal deny-lists and
    // skip probing the Kitty/CSI-u keyboard support.
    entries.erase(std::remove_if(entries.begin(), entries.end(), [](const std::wstring &entry) {
                      return environmentEntryMatches(entry, L"WT_SESSION");
                  }),
                  entries.end());

    for (auto it = overrides.cbegin(); it != overrides.cend(); ++it) {
        const std::wstring name = it.key().toStdWString();
        const std::wstring value = it.value().toStdWString();
        replace(name.c_str(), value.c_str());
    }

    std::sort(entries.begin(), entries.end(), [](const std::wstring &left, const std::wstring &right) {
        return _wcsicmp(left.c_str(), right.c_str()) < 0;
    });

    std::vector<wchar_t> result;
    for (const std::wstring &entry : entries) {
        result.insert(result.end(), entry.cbegin(), entry.cend());
        result.push_back(L'\0');
    }
    result.push_back(L'\0');
    return result;
}

UniqueHandle createPipeServer(const QString &name,
                              SECURITY_ATTRIBUTES *security,
                              QString *errorMessage) {
    const std::wstring nativeName = name.toStdWString();
    HANDLE handle = CreateNamedPipeW(nativeName.c_str(), PIPE_ACCESS_DUPLEX | FILE_FLAG_FIRST_PIPE_INSTANCE,
                                     PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT, 1, kPipeBufferBytes,
                                     kPipeBufferBytes, kPipeDefaultTimeoutMs, security);
    if (handle == INVALID_HANDLE_VALUE) {
        if (errorMessage) {
            *errorMessage = windowsErrorMessage(QStringLiteral("Could not create terminal named pipe."));
        }
        return {};
    }
    return UniqueHandle(handle);
}

// Overlapped: its reads and writes complete on the thread pool.
UniqueHandle openPipeClient(const QString &name, DWORD access, QString *errorMessage) {
    const std::wstring nativeName = name.toStdWString();
    HANDLE handle = CreateFileW(nativeName.c_str(), access, 0, nullptr, OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        if (errorMessage) {
            *errorMessage = windowsErrorMessage(QStringLiteral("Could not connect terminal named pipe."));
        }
        return {};
    }
    return UniqueHandle(handle);
}

bool finishPipeConnection(HANDLE server, QString *errorMessage) {
    if (ConnectNamedPipe(server, nullptr)) {
        return true;
    }
    if (GetLastError() == ERROR_PIPE_CONNECTED) {
        return true;
    }
    if (errorMessage) {
        *errorMessage = windowsErrorMessage(QStringLiteral("Could not finish terminal pipe connection."));
    }
    return false;
}

bool StartupInfo::attachPseudoConsole(HPCON pseudoConsole, HANDLE job, QString *errorMessage) {
    const DWORD attributeCount = job ? 2 : 1;
    SIZE_T bytesRequired = 0;
    InitializeProcThreadAttributeList(nullptr, attributeCount, 0, &bytesRequired);
    m_info.lpAttributeList = static_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(
        HeapAlloc(GetProcessHeap(), 0, bytesRequired));
    if (!m_info.lpAttributeList) {
        if (errorMessage) {
            *errorMessage = QStringLiteral("Could not allocate terminal startup information.");
        }
        return false;
    }

    if (!InitializeProcThreadAttributeList(m_info.lpAttributeList, attributeCount, 0, &bytesRequired)) {
        if (errorMessage) {
            *errorMessage = windowsErrorMessage(QStringLiteral("Could not initialize terminal startup attributes."));
        }
        return false;
    }

    if (!UpdateProcThreadAttribute(m_info.lpAttributeList,
                                   0,
                                   PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE,
                                   pseudoConsole,
                                   sizeof(pseudoConsole),
                                   nullptr,
                                   nullptr)) {
        if (errorMessage) {
            *errorMessage = windowsErrorMessage(QStringLiteral("Could not configure pseudo console."));
        }
        return false;
    }

    if (job) {
        m_jobList[0] = job;
        if (!UpdateProcThreadAttribute(m_info.lpAttributeList,
                                       0,
                                       PROC_THREAD_ATTRIBUTE_JOB_LIST,
                                       m_jobList,
                                       sizeof(m_jobList),
                                       nullptr,
                                       nullptr)) {
            // Non-fatal: the job is assigned after CreateProcess instead.
            DeleteProcThreadAttributeList(m_info.lpAttributeList);
            HeapFree(GetProcessHeap(), 0, m_info.lpAttributeList);
            m_info.lpAttributeList = nullptr;
            return attachPseudoConsole(pseudoConsole, nullptr, errorMessage);
        }
    }
    return true;
}
}

namespace span::terminal {

namespace {
constexpr qsizetype kMaximumPendingWriteBytes = 1024 * 1024;
constexpr DWORD kReadChunkBytes = 64 * 1024;
}

// A session owns no thread: pipe reads and writes, the exit wait and resizes
// run as thread-pool callbacks. Each has at most one callback in flight, so
// output stays ordered and writes stay FIFO. shutdown() returns only after the
// last callback, so none outlives the handles it uses.
struct WinConPtyProcess::Impl final {
    HPCON pseudoConsole = nullptr;
    UniqueHandle process;
    DWORD processId = 0;
    UniqueHandle inputWrite;
    UniqueHandle outputRead;
    OutputCallback outputCallback;
    ExitCallback exitCallback;
    std::unique_ptr<ConPtyChildJob> childJob;
    std::atomic_bool running = false;
    std::atomic_bool exitNotified = false;
    std::mutex pseudoConsoleMutex;

    PTP_IO readIo = nullptr;
    PTP_IO writeIo = nullptr;
    HANDLE exitWait = nullptr;
    OVERLAPPED readOverlapped{};
    OVERLAPPED writeOverlapped{};
    std::vector<char> readBuffer = std::vector<char>(kReadChunkBytes);
    // Waited on by the inline reads that drain what one producer write left.
    UniqueHandle drainEvent;
    // Set once the reader has delivered its last chunk.
    UniqueHandle readerDone;

    // Orders issuing reads and writes against shutdown's cancel.
    std::mutex ioMutex;
    bool stopping = false;
    // The child exited or the input pipe broke: output still drains.
    bool writesClosed = false;
    struct PendingWrite {
        QByteArray bytes;
        TerminalInputKind kind;
    };
    std::deque<PendingWrite> pendingWrites;
    QByteArray writeInFlight;
    qsizetype writeOffset = 0;
    bool writing = false;
    bool bracketedPasteInFlight = false;
    qsizetype pendingWriteBytes = 0;

    std::mutex resizeMutex;
    std::optional<COORD> pendingResize;
    bool resizing = false;
    bool resizeStopping = false;

    void closePseudoConsole() {
        HPCON handle = nullptr;
        {
            std::lock_guard<std::mutex> lock(pseudoConsoleMutex);
            handle = std::exchange(pseudoConsole, nullptr);
        }
        if (handle) {
            conPtyApi().close(handle);
        }
    }

    void notifyExit(DWORD exitCode) {
        if (!exitNotified.exchange(true) && exitCallback) {
            exitCallback(quint32(exitCode));
        }
    }

    // ioMutex held.
    void issueRead()
    {
        if (stopping) {
            SetEvent(readerDone.get());
            return;
        }
        StartThreadpoolIo(readIo);
        readOverlapped = {};
        if (!ReadFile(outputRead.get(), readBuffer.data(), kReadChunkBytes, nullptr, &readOverlapped)
            && GetLastError() != ERROR_IO_PENDING) {
            CancelThreadpoolIo(readIo);
            SetEvent(readerDone.get());
        }
    }

    // Reads bytes PeekNamedPipe reported, so it completes at once. The low
    // bit of the event keeps this completion off the thread pool.
    bool readAvailable(char *data, DWORD size, DWORD *count)
    {
        OVERLAPPED overlapped{};
        overlapped.hEvent = reinterpret_cast<HANDLE>(reinterpret_cast<ULONG_PTR>(drainEvent.get()) | 1);
        if (!ReadFile(outputRead.get(), data, size, nullptr, &overlapped) && GetLastError() != ERROR_IO_PENDING)
            return false;
        return GetOverlappedResult(outputRead.get(), &overlapped, count, TRUE) && *count > 0;
    }

    static void CALLBACK readCompleted(PTP_CALLBACK_INSTANCE, PVOID context, PVOID, ULONG result, ULONG_PTR bytes,
                                       PTP_IO)
    {
        auto *self = static_cast<Impl *>(context);
        if (result != NO_ERROR || bytes == 0) {
            SetEvent(self->readerDone.get());
            return;
        }
        const HANDLE handle = self->outputRead.get();
        char *buffer = self->readBuffer.data();
        DWORD filled = DWORD(bytes);
        bool open = true;
        // conhost flushes each painted frame with one write. Collect what
        // that write already put in the pipe, so the chunk ends where the
        // pipe ran dry: the only place the Core may present the screen.
        DWORD available = 0;
        while (filled < kReadChunkBytes && PeekNamedPipe(handle, nullptr, 0, nullptr, &available, nullptr)
               && available > 0) {
            DWORD count = 0;
            if (!self->readAvailable(buffer + filled, std::min<DWORD>(available, kReadChunkBytes - filled), &count)) {
                open = false;
                available = 0;
                break;
            }
            filled += count;
        }
        if (open && filled == kReadChunkBytes && !PeekNamedPipe(handle, nullptr, 0, nullptr, &available, nullptr)) {
            available = 0;
        }
        if (self->outputCallback) {
            self->outputCallback(QByteArray(buffer, int(filled)), available == 0);
        }
        if (!open) {
            SetEvent(self->readerDone.get());
            return;
        }
        const std::lock_guard<std::mutex> lock(self->ioMutex);
        self->issueRead();
    }

    // ioMutex held.
    void pumpWrites()
    {
        if (writing || stopping || writesClosed || pendingWrites.empty()) return;
        PendingWrite next = std::move(pendingWrites.front());
        pendingWrites.pop_front();
        if (next.kind == TerminalInputKind::PasteBegin) bracketedPasteInFlight = true;
        if (next.kind == TerminalInputKind::PasteEnd) bracketedPasteInFlight = false;
        pendingWriteBytes -= next.bytes.size();
        writeInFlight = std::move(next.bytes);
        writeOffset = 0;
        writing = true;
        issueWrite();
    }

    // ioMutex held.
    void issueWrite()
    {
        StartThreadpoolIo(writeIo);
        writeOverlapped = {};
        const DWORD remaining = DWORD(std::min<qsizetype>(writeInFlight.size() - writeOffset, MAXDWORD));
        if (!WriteFile(inputWrite.get(), writeInFlight.constData() + writeOffset, remaining, nullptr, &writeOverlapped)
            && GetLastError() != ERROR_IO_PENDING) {
            CancelThreadpoolIo(writeIo);
            closeWrites();
        }
    }

    // ioMutex held.
    void closeWrites()
    {
        writing = false;
        writesClosed = true;
        writeInFlight.clear();
    }

    static void CALLBACK writeCompleted(PTP_CALLBACK_INSTANCE, PVOID context, PVOID, ULONG result, ULONG_PTR bytes,
                                        PTP_IO)
    {
        auto *self = static_cast<Impl *>(context);
        const std::lock_guard<std::mutex> lock(self->ioMutex);
        if (result != NO_ERROR || bytes == 0) {
            self->closeWrites();
            return;
        }
        self->writeOffset += qsizetype(bytes);
        if (self->writeOffset < self->writeInFlight.size() && !self->stopping) {
            self->issueWrite();
            return;
        }
        self->writing = false;
        self->writeInFlight.clear();
        self->pumpWrites();
    }

    // Exit is reported after the last output, which conhost flushes when its
    // pseudo console closes.
    static void CALLBACK processExited(PVOID context, BOOLEAN)
    {
        auto *self = static_cast<Impl *>(context);
        DWORD exitCode = 0;
        GetExitCodeProcess(self->process.get(), &exitCode);
        self->running = false;
        {
            const std::lock_guard<std::mutex> lock(self->ioMutex);
            self->writesClosed = true;
        }
        self->closePseudoConsole();
        WaitForSingleObject(self->readerDone.get(), INFINITE);
        self->notifyExit(exitCode);
    }

    // Only the newest request waits; one runs at a time. The context owns a
    // reference: shutdown() does not wait for this callback.
    static void CALLBACK resizeWork(PTP_CALLBACK_INSTANCE, PVOID context)
    {
        const std::unique_ptr<std::shared_ptr<Impl>> owner(static_cast<std::shared_ptr<Impl> *>(context));
        Impl *self = owner->get();
        for (;;) {
            COORD size{};
            {
                const std::lock_guard<std::mutex> lock(self->resizeMutex);
                if (!self->pendingResize || self->resizeStopping) {
                    self->resizing = false;
                    return;
                }
                size = *self->pendingResize;
                self->pendingResize.reset();
            }
            // Under the lock: closing the console waits for the call.
            HRESULT result = S_OK;
            {
                const std::lock_guard<std::mutex> lock(self->pseudoConsoleMutex);
                if (self->pseudoConsole) result = conPtyApi().resize(self->pseudoConsole, size);
            }
            if (FAILED(result)) {
                qWarning("ResizePseudoConsole(%d x %d) failed (0x%08lx).", size.X, size.Y,
                         static_cast<unsigned long>(result));
            }
        }
    }

    void shutdown() {
        {
            const std::lock_guard<std::mutex> lock(ioMutex);
            stopping = true;
            pendingWrites.clear();
            pendingWriteBytes = 0;
        }
        {
            const std::lock_guard<std::mutex> lock(resizeMutex);
            resizeStopping = true;
            pendingResize.reset();
        }

        // Before draining I/O, so no descendant keeps ConPTY alive.
        childJob.reset();
        if (process) {
            DWORD exitCode = 0;
            if (GetExitCodeProcess(process.get(), &exitCode) && exitCode == STILL_ACTIVE) {
                TerminateProcess(process.get(), 1);
            }
        }

        // No read or write is issued after `stopping`; cancel the ones in
        // flight and wait for their callbacks.
        if (outputRead) CancelIoEx(outputRead.get(), nullptr);
        if (inputWrite) CancelIoEx(inputWrite.get(), nullptr);
        for (PTP_IO *io : {&readIo, &writeIo}) {
            if (*io) {
                WaitForThreadpoolIoCallbacks(*io, FALSE);
                CloseThreadpoolIo(*io);
                *io = nullptr;
            }
        }
        if (readerDone) SetEvent(readerDone.get());

        // Nothing drains the output pipe any more: close it before
        // ClosePseudoConsole, or conhost can block on a full pipe.
        inputWrite.reset();
        outputRead.reset();

        // Releases conhost even when a descendant ignored termination or
        // escaped the job.
        closePseudoConsole();

        if (exitWait) {
            UnregisterWaitEx(exitWait, INVALID_HANDLE_VALUE);
            exitWait = nullptr;
        }
        process.reset();
        running = false;
    }
};

WinConPtyProcess::WinConPtyProcess() = default;

WinConPtyProcess::~WinConPtyProcess() {
    stop();
}

bool WinConPtyProcess::usesBundledConPty() {
    return conPtyApi().bundled;
}

void WinConPtyProcess::setPreferSystemConPty(bool prefer) {
    g_preferSystemConPty.store(prefer);
}

QString WinConPtyProcess::defaultShellCommand() {
    return findShell();
}

bool WinConPtyProcess::start(const StartOptions &options,
                          OutputCallback outputCallback,
                          ExitCallback exitCallback,
                          QString *errorMessage) {
    stop();

    OwnerOnlyPipeSecurity pipeSecurity;
    if (!pipeSecurity.ready()) {
        if (errorMessage) {
            *errorMessage = windowsErrorMessage(
                QStringLiteral("Could not build terminal pipe security descriptor."));
        }
        return false;
    }

    UniqueHandle inputServer;
    UniqueHandle inputWrite;
    UniqueHandle outputRead;
    UniqueHandle outputServer;

    const QString inputName = pipeName(QStringLiteral("in"));
    const QString outputName = pipeName(QStringLiteral("out"));
    inputServer = createPipeServer(inputName, pipeSecurity.get(), errorMessage);
    if (!inputServer) {
        return false;
    }
    outputServer = createPipeServer(outputName, pipeSecurity.get(), errorMessage);
    if (!outputServer) {
        return false;
    }

    auto childJob = std::make_unique<ConPtyChildJob>();
    HANDLE childJobHandle = childJob->handle();
    if (!childJobHandle) {
        if (errorMessage) {
            *errorMessage = QStringLiteral(
                "Could not create a Windows job for the terminal process tree.");
        }
        return false;
    }

    COORD size{};
    size.X = SHORT(std::clamp(options.columns, 1, int(SHRT_MAX)));
    size.Y = SHORT(std::clamp(options.rows, 1, int(SHRT_MAX)));
    HPCON pseudoConsole = nullptr;
    std::vector<DWORD> childrenBefore;
    {
        std::lock_guard<std::mutex> lock(pseudoConsoleCreateMutex());
        childrenBefore = childProcessIds(GetCurrentProcessId());
        const HRESULT ptyResult = conPtyApi().create(size, inputServer.get(), outputServer.get(), 0, &pseudoConsole);
        if (FAILED(ptyResult)) {
            if (errorMessage) {
                *errorMessage = QStringLiteral("Could not create pseudo console (0x%1).")
                                    .arg(quint32(ptyResult), 8, 16, QLatin1Char('0'));
            }
            return false;
        }
        assignNewChildrenToJob(childJobHandle, childrenBefore);
    }

    inputWrite = openPipeClient(inputName, GENERIC_WRITE, errorMessage);
    outputRead = openPipeClient(outputName, GENERIC_READ, errorMessage);
    if (!inputWrite || !outputRead ||
        !finishPipeConnection(inputServer.get(), errorMessage) ||
        !finishPipeConnection(outputServer.get(), errorMessage)) {
        conPtyApi().close(pseudoConsole);
        return false;
    }

    StartupInfo startupInfo;
    if (!startupInfo.attachPseudoConsole(pseudoConsole, childJobHandle, errorMessage)) {
        conPtyApi().close(pseudoConsole);
        return false;
    }

    QString commandLine = options.commandLine.trimmed();
    if (commandLine.isEmpty()) {
        commandLine = defaultShellCommand();
    }
    std::wstring mutableCommand = commandLine.toStdWString();
    const QString requestedDirectory = options.workingDirectory.trimmed();
    QString workingDirectory;
    if (requestedDirectory.isEmpty()) {
        workingDirectory = QDir::homePath();
    } else if (QFileInfo(requestedDirectory).isDir()) {
        workingDirectory = QDir::toNativeSeparators(requestedDirectory);
    } else {
        if (errorMessage) {
            *errorMessage = QStringLiteral("Working directory is missing or not a directory: %1")
                                .arg(requestedDirectory);
        }
        conPtyApi().close(pseudoConsole);
        return false;
    }
    std::wstring nativeDirectory = workingDirectory.toStdWString();
    std::vector<wchar_t> environment = terminalEnvironment(options.colorFgBg, options.environment);

    PROCESS_INFORMATION processInfo{};
    // Suspended until it is in the job, so no grandchild races out of it.
    const DWORD creationFlags = EXTENDED_STARTUPINFO_PRESENT
                                | CREATE_UNICODE_ENVIRONMENT
                                | CREATE_SUSPENDED;
    const BOOL created = CreateProcessW(nullptr,
                                        mutableCommand.data(),
                                        nullptr,
                                        nullptr,
                                        FALSE,
                                        creationFlags,
                                        environment.data(),
                                        nativeDirectory.c_str(),
                                        &startupInfo.get()->StartupInfo,
                                        &processInfo);
    if (!created) {
        if (errorMessage) {
            *errorMessage = windowsErrorMessage(QStringLiteral("Could not start shell:"));
        }
        conPtyApi().close(pseudoConsole);
        return false;
    }

    UniqueHandle process(processInfo.hProcess);
    UniqueHandle processThread(processInfo.hThread);

    // JOB_LIST may already own it. Never resume a shell outside the job.
    BOOL alreadyInJob = FALSE;
    if (!IsProcessInJob(process.get(), childJobHandle, &alreadyInJob)) {
        alreadyInJob = FALSE;
    }
    if (!alreadyInJob && !childJob->assign(process.get())) {
        if (errorMessage) {
            *errorMessage = windowsErrorMessage(
                QStringLiteral("Could not assign the shell to its terminal job:"));
        }
        TerminateProcess(process.get(), 1);
        conPtyApi().close(pseudoConsole);
        return false;
    }

    if (ResumeThread(processThread.get()) == static_cast<DWORD>(-1)) {
        if (errorMessage) {
            *errorMessage = windowsErrorMessage(QStringLiteral("Could not resume shell:"));
        }
        TerminateProcess(process.get(), 1);
        conPtyApi().close(pseudoConsole);
        return false;
    }
    inputServer.reset();
    outputServer.reset();

    auto impl = std::make_shared<Impl>();
    impl->pseudoConsole = pseudoConsole;
    impl->process = std::move(process);
    impl->processId = processInfo.dwProcessId;
    impl->inputWrite = std::move(inputWrite);
    impl->outputRead = std::move(outputRead);
    impl->childJob = std::move(childJob);
    impl->outputCallback = std::move(outputCallback);
    impl->exitCallback = std::move(exitCallback);
    impl->drainEvent = UniqueHandle(CreateEventW(nullptr, TRUE, FALSE, nullptr));
    impl->readerDone = UniqueHandle(CreateEventW(nullptr, TRUE, FALSE, nullptr));
    impl->readIo = CreateThreadpoolIo(impl->outputRead.get(), &Impl::readCompleted, impl.get(), nullptr);
    impl->writeIo = CreateThreadpoolIo(impl->inputWrite.get(), &Impl::writeCompleted, impl.get(), nullptr);
    // Before the wait: an instant exit must not be overwritten.
    impl->running = true;
    if (!impl->drainEvent || !impl->readerDone || !impl->readIo || !impl->writeIo
        || !RegisterWaitForSingleObject(&impl->exitWait, impl->process.get(), &Impl::processExited, impl.get(),
                                        INFINITE, WT_EXECUTEONLYONCE | WT_EXECUTELONGFUNCTION)) {
        if (errorMessage) {
            *errorMessage = windowsErrorMessage(QStringLiteral("Could not start terminal I/O:"));
        }
        impl->shutdown();
        return false;
    }
    {
        const std::lock_guard<std::mutex> lock(impl->ioMutex);
        impl->issueRead();
    }
    std::atomic_store(&m_impl, std::move(impl));
    return true;
}

bool WinConPtyProcess::write(const QByteArray &bytes, TerminalInputKind kind) {
    const auto impl = std::atomic_load(&m_impl);
    if (!impl || bytes.isEmpty() || !impl->running) {
        return false;
    }
    const std::lock_guard<std::mutex> lock(impl->ioMutex);
    if (impl->stopping || impl->writesClosed || bytes.size() > kMaximumPendingWriteBytes
        || impl->pendingWriteBytes + bytes.size() > kMaximumPendingWriteBytes) {
        return false;
    }
    impl->pendingWrites.push_back({bytes, kind});
    impl->pendingWriteBytes += bytes.size();
    impl->pumpWrites();
    return true;
}

bool WinConPtyProcess::cancelPendingWritesAndInterrupt()
{
    const auto impl = std::atomic_load(&m_impl);
    if (!impl || !impl->running) {
        return false;
    }
    const std::lock_guard<std::mutex> lock(impl->ioMutex);
    if (impl->stopping || impl->writesClosed) {
        return false;
    }
    QByteArray control;
    if (impl->bracketedPasteInFlight) control.append("\x1b[201~", 6);
    control.append(char(0x03));
    impl->bracketedPasteInFlight = false;
    impl->pendingWrites.erase(
        std::remove_if(impl->pendingWrites.begin(), impl->pendingWrites.end(),
                       [](const Impl::PendingWrite &write) { return write.kind != TerminalInputKind::Protocol; }),
        impl->pendingWrites.end());
    impl->pendingWriteBytes = control.size();
    for (const auto &write : impl->pendingWrites) impl->pendingWriteBytes += write.bytes.size();
    impl->pendingWrites.push_front({std::move(control), TerminalInputKind::Protocol});
    impl->pumpWrites();
    return true;
}

void WinConPtyProcess::clearScreen()
{
    const auto impl = std::atomic_load(&m_impl);
    if (!impl || !impl->running || !conPtyApi().clear) return;
    // A signal-pipe write: it does not wait for conhost.
    std::lock_guard<std::mutex> lock(impl->pseudoConsoleMutex);
    if (impl->pseudoConsole) conPtyApi().clear(impl->pseudoConsole, TRUE);
}

void WinConPtyProcess::requestResize(int columns, int rows)
{
    const auto impl = std::atomic_load(&m_impl);
    if (!impl || !impl->running) return;
    const std::lock_guard<std::mutex> lock(impl->resizeMutex);
    if (impl->resizeStopping) return;
    impl->pendingResize =
        COORD{SHORT(std::clamp(columns, 1, int(SHRT_MAX))), SHORT(std::clamp(rows, 1, int(SHRT_MAX)))};
    if (impl->resizing) return;
    auto *owner = new std::shared_ptr<Impl>(impl);
    if (TrySubmitThreadpoolCallback(&Impl::resizeWork, owner, nullptr)) {
        impl->resizing = true;
    } else {
        delete owner;
        impl->pendingResize.reset();
        qWarning("Could not queue a pseudo console resize.");
    }
}

void WinConPtyProcess::stop() {
    if (const auto impl = std::atomic_exchange(&m_impl, std::shared_ptr<Impl>())) impl->shutdown();
}

bool WinConPtyProcess::isRunning() const {
    const auto impl = std::atomic_load(&m_impl);
    return impl && impl->running;
}

qsizetype WinConPtyProcess::pendingWriteBytes() const {
    const auto impl = std::atomic_load(&m_impl);
    if (!impl) {
        return 0;
    }
    const std::lock_guard<std::mutex> lock(impl->ioMutex);
    return impl->pendingWriteBytes + impl->writeInFlight.size() - impl->writeOffset;
}

quint32 WinConPtyProcess::processId() const {
    const auto impl = std::atomic_load(&m_impl);
    return impl && impl->running ? quint32(impl->processId) : 0;
}

QList<quint32> WinConPtyProcess::jobProcessIds() const {
    const auto impl = std::atomic_load(&m_impl);
    if (!impl || !impl->running || !impl->childJob) {
        return {};
    }
    return impl->childJob->processIds();
}

} // namespace span::terminal
