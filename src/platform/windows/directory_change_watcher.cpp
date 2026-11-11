#include "platform/windows/directory_change_watcher.h"

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
};

DirectoryChangeWatcher::DirectoryChangeWatcher(QObject *parent) : QObject(parent), m_impl(std::make_unique<Impl>())
{
    connect(&m_impl->notifier, &QWinEventNotifier::activated, this, &DirectoryChangeWatcher::readCompleted);
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
    if (!startRead()) {
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

bool DirectoryChangeWatcher::startRead()
{
    // Manual-reset event: clear it so the notifier does not fire again
    // before this read completes.
    ResetEvent(m_impl->event);
    m_impl->overlapped = {};
    m_impl->overlapped.hEvent = m_impl->event;
    m_impl->readPending = ReadDirectoryChangesW(m_impl->directory, m_impl->buffer.data(), kBufferBytes, TRUE,
                                                kNotifyFilter, nullptr, &m_impl->overlapped, nullptr)
        != FALSE;
    return m_impl->readPending;
}

void DirectoryChangeWatcher::readCompleted()
{
    if (!m_impl->readPending) return;
    m_impl->readPending = false;
    DWORD bytes = 0;
    const bool completed = GetOverlappedResult(m_impl->directory, &m_impl->overlapped, &bytes, FALSE) != FALSE;
    const DWORD error = completed ? ERROR_SUCCESS : GetLastError();
    if (error == ERROR_IO_INCOMPLETE) {
        m_impl->readPending = true;
        return;
    }

    // Copy the names out before the next read reuses the buffer; zero bytes
    // (or ERROR_NOTIFY_ENUM_DIR) is an overflow and stays an empty list.
    QStringList paths;
    if (completed && bytes > 0) {
        const auto *base = reinterpret_cast<const char *>(m_impl->buffer.data());
        for (DWORD offset = 0;;) {
            const auto *info = reinterpret_cast<const FILE_NOTIFY_INFORMATION *>(base + offset);
            paths.append(QString::fromWCharArray(info->FileName, int(info->FileNameLength / sizeof(WCHAR)))
                             .replace(QLatin1Char('\\'), QLatin1Char('/')));
            if (info->NextEntryOffset == 0) break;
            offset += info->NextEntryOffset;
        }
    }

    // A failed read other than an overflow (the root was deleted or renamed)
    // ends the watch; the change is still reported.
    if ((!completed && error != ERROR_NOTIFY_ENUM_DIR) || !startRead()) stop();
    emit changed(paths);
}

} // namespace span::platform
