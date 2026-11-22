#include "application/diagnostics/resource_reporter.h"

#include "monitoring/platform_resource_sampler.h"
#include "workspace/file_tree_model.h"
#include "terminal/api/terminal_manager.h"

#include <QDateTime>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QQuickWindow>
#include <QThread>
#include <QTimer>

#include <algorithm>

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

    const OwnProcessCounters counters = ownProcessCounters();
    if (counters.privateBytes >= 0) snapshot.insert(QStringLiteral("privateBytes"), counters.privateBytes);
    if (counters.workingSetBytes >= 0) snapshot.insert(QStringLiteral("workingSetBytes"), counters.workingSetBytes);
    if (counters.handles >= 0) snapshot.insert(QStringLiteral("handles"), counters.handles);
    snapshot.insert(QStringLiteral("threads"), std::max(counters.threads, 0));
    m_file->write(QJsonDocument(snapshot).toJson(QJsonDocument::Compact));
    m_file->write("\n");
    m_file->flush();
}

} // namespace span::application::diagnostics
