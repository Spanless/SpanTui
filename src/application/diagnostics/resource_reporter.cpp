#include "application/diagnostics/resource_reporter.h"

#include "workspace/file_tree_model.h"
#include "terminal/api/terminal_manager.h"

#include <QDateTime>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QQuickWindow>
#include <QThread>
#include <QTimer>

#include <windows.h>
#include <psapi.h>
#include <tlhelp32.h>

namespace span::application::diagnostics {
namespace {
constexpr int kSnapshotIntervalMs = 5000;
}

ResourceReporter::ResourceReporter(span::terminal::TerminalManager *terminalManager,
                                   FileTreeModel *fileTreeModel,
                                   QObject *parent)
    : QObject(parent), m_terminalManager(terminalManager), m_fileTreeModel(fileTreeModel),
      m_timer(new QTimer(this)) {
    m_timer->setInterval(kSnapshotIntervalMs);
    connect(m_timer, &QTimer::timeout, this, &ResourceReporter::writeSnapshot);
}

ResourceReporter::~ResourceReporter() = default;

bool ResourceReporter::start(const QString &path, QQuickWindow *window, QString *errorMessage) {
    auto *file = new QFile(path, this);
    if (!file->open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text)) {
        if (errorMessage) {
            *errorMessage = file->errorString();
        }
        file->deleteLater();
        return false;
    }
    m_file = file;
    m_window = window;
    writeSnapshot();
    m_timer->start();
    return true;
}

void ResourceReporter::writeSnapshot() {
    if (!m_file || !m_terminalManager || !m_file->isOpen()) {
        return;
    }
    QJsonObject snapshot;
    snapshot.insert(QStringLiteral("timestampUtc"), QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs));
    snapshot.insert(QStringLiteral("terminalSessions"), m_terminalManager->sessionCount());
    snapshot.insert(QStringLiteral("runningTerminals"), m_terminalManager->runningSessionCount());
    snapshot.insert(QStringLiteral("dormantTerminals"),
                    m_terminalManager->sessionCount() - m_terminalManager->runningSessionCount());
    snapshot.insert(QStringLiteral("pendingInputBytes"), qint64(m_terminalManager->pendingInputBytes()));
    snapshot.insert(QStringLiteral("pendingOutputBytes"), qint64(m_terminalManager->pendingOutputBytes()));
    snapshot.insert(QStringLiteral("fileTreeNodes"), m_fileTreeModel ? m_fileTreeModel->nodeCount() : 0);
    snapshot.insert(QStringLiteral("windowActive"), m_window && m_window->isActive());
    snapshot.insert(QStringLiteral("windowVisible"), m_window && m_window->isVisible());

    PROCESS_MEMORY_COUNTERS_EX counters{};
    if (GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS *>(&counters),
                             sizeof(counters))) {
        snapshot.insert(QStringLiteral("privateBytes"), qint64(counters.PrivateUsage));
        snapshot.insert(QStringLiteral("workingSetBytes"), qint64(counters.WorkingSetSize));
    }
    DWORD handles = 0;
    if (GetProcessHandleCount(GetCurrentProcess(), &handles)) {
        snapshot.insert(QStringLiteral("handles"), int(handles));
    }
    int threadCount = 0;
    HANDLE threadSnapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (threadSnapshot != INVALID_HANDLE_VALUE) {
        THREADENTRY32 entry{};
        entry.dwSize = sizeof(entry);
        for (BOOL found = Thread32First(threadSnapshot, &entry); found; found = Thread32Next(threadSnapshot, &entry)) {
            if (entry.th32OwnerProcessID == GetCurrentProcessId()) {
                ++threadCount;
            }
        }
        CloseHandle(threadSnapshot);
    }
    snapshot.insert(QStringLiteral("threads"), threadCount);
    m_file->write(QJsonDocument(snapshot).toJson(QJsonDocument::Compact));
    m_file->write("\n");
    m_file->flush();
}

} // namespace span::application::diagnostics
