#include "platform/directory_change_watcher.h"

#include <QDir>
#include <QWinEventNotifier>

#include <windows.h>

#include <vector>

namespace span::platform {

namespace {
// One read's worth of notifications (also the limit for network shares). A
// burst that does not fit arrives as an overflow, which reports "anything".
constexpr DWORD kBufferBytes = 64 * 1024;
constexpr DWORD kNotifyFilter = FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_DIR_NAME | FILE_NOTIFY_CHANGE_SIZE
    | FILE_NOTIFY_CHANGE_LAST_WRITE;
} // namespace

struct DirectoryChangeWatcher::Impl {
    HANDLE directory = INVALID_HANDLE_VALUE;
    HANDLE event = nullptr;
    OVERLAPPED overlapped{};
    // FILE_NOTIFY_INFORMATION records must be DWORD aligned.
    std::vector<DWORD> buffer = std::vector<DWORD>(kBufferBytes / sizeof(DWORD));
    QWinEventNotifier notifier;
    bool readPending = false;

    bool startRead();
    // The names of a finished read, or false while it is still running.
    // `ended`: the read failed for a reason other than an overflow (the root
    // was deleted or renamed), so the watch cannot go on.
    bool takeCompleted(QStringList *paths, bool *ended);
};

DirectoryChangeWatcher::DirectoryChangeWatcher(QObject *parent) : QObject(parent), m_impl(std::make_unique<Impl>())
{
    connect(&m_impl->notifier, &QWinEventNotifier::activated, this, [this]() {
        QStringList paths;
        bool ended = false;
        if (!m_impl->takeCompleted(&paths, &ended)) return;
        // A watch that cannot go on still reports the change that ended it.
        if (ended || !m_impl->startRead()) stop();
        emit changed(paths);
    });
}

DirectoryChangeWatcher::~DirectoryChangeWatcher()
{
    stop();
}

bool DirectoryChangeWatcher::watch(const QString &root)
{
    stop();
    if (root.isEmpty()) return false;
    const std::wstring path = QDir::toNativeSeparators(root).toStdWString();
    m_impl->directory =
        CreateFileW(path.c_str(), FILE_LIST_DIRECTORY, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                    OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OVERLAPPED, nullptr);
    if (m_impl->directory == INVALID_HANDLE_VALUE) return false;
    m_impl->event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!m_impl->event) {
        stop();
        return false;
    }
    m_impl->notifier.setHandle(m_impl->event);
    m_impl->notifier.setEnabled(true);
    if (!m_impl->startRead()) {
        stop();
        return false;
    }
    return true;
}

void DirectoryChangeWatcher::stop()
{
    m_impl->notifier.setEnabled(false);
    if (m_impl->directory != INVALID_HANDLE_VALUE) {
        if (m_impl->readPending) {
            // The kernel writes into the buffer until the cancelled read
            // completes; wait for it before the buffer can be reused.
            CancelIoEx(m_impl->directory, &m_impl->overlapped);
            DWORD bytes = 0;
            GetOverlappedResult(m_impl->directory, &m_impl->overlapped, &bytes, TRUE);
        }
        CloseHandle(m_impl->directory);
        m_impl->directory = INVALID_HANDLE_VALUE;
    }
    m_impl->readPending = false;
    if (m_impl->event) {
        CloseHandle(m_impl->event);
        m_impl->event = nullptr;
    }
}

bool DirectoryChangeWatcher::watching() const
{
    return m_impl->readPending;
}

bool DirectoryChangeWatcher::Impl::startRead()
{
    // Manual-reset event: clear it so the notifier does not fire again
    // before this read completes.
    ResetEvent(event);
    overlapped = {};
    overlapped.hEvent = event;
    readPending = ReadDirectoryChangesW(directory, buffer.data(), kBufferBytes, TRUE, kNotifyFilter, nullptr,
                                        &overlapped, nullptr)
        != FALSE;
    return readPending;
}

bool DirectoryChangeWatcher::Impl::takeCompleted(QStringList *paths, bool *ended)
{
    if (!readPending) return false;
    readPending = false;
    DWORD bytes = 0;
    const bool completed = GetOverlappedResult(directory, &overlapped, &bytes, FALSE) != FALSE;
    const DWORD error = completed ? ERROR_SUCCESS : GetLastError();
    if (error == ERROR_IO_INCOMPLETE) {
        readPending = true;
        return false;
    }

    // Copy the names out before the next read reuses the buffer; zero bytes
    // (or ERROR_NOTIFY_ENUM_DIR) is an overflow and stays an empty list.
    if (completed && bytes > 0) {
        const auto *base = reinterpret_cast<const char *>(buffer.data());
        for (DWORD offset = 0;;) {
            const auto *info = reinterpret_cast<const FILE_NOTIFY_INFORMATION *>(base + offset);
            paths->append(QString::fromWCharArray(info->FileName, int(info->FileNameLength / sizeof(WCHAR)))
                              .replace(QLatin1Char('\\'), QLatin1Char('/')));
            if (info->NextEntryOffset == 0) break;
            offset += info->NextEntryOffset;
        }
    }
    *ended = !completed && error != ERROR_NOTIFY_ENUM_DIR;
    return true;
}

} // namespace span::platform
