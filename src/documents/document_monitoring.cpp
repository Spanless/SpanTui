#include "documents/document_manager.h"

#include <QFileInfo>
#include <QFileSystemWatcher>
#include <QFutureWatcher>
#include <QtConcurrent>

namespace {
constexpr qint64 kAsyncRefreshBytes = 1024 * 1024;
}

void DocumentManager::watchPath(const QString &path) {
    if (QFileInfo::exists(path) && !m_watcher.files().contains(path)) m_watcher.addPath(path);
}

void DocumentManager::handleExternalChange(const QString &path) {
    const int row = indexForPath(path);
    if (row < 0) return;
    const QString documentPath = m_documents.at(row).path;
    const qulonglong documentId = m_documents.at(row).id;
    if (QFileInfo(documentPath).size() >= kAsyncRefreshBytes) {
        beginLoading(tr("Refreshing %1").arg(m_documents.at(row).name));
        markDocumentLoading(documentId, true);
        const quint64 generation =
            m_externalChangeGenerations.value(normalizedPath(documentPath));
        auto *watcher = new QFutureWatcher<ReadResult>(this);
        connect(watcher, &QFutureWatcher<ReadResult>::finished, this,
                [this, watcher, documentPath, documentId, generation]() {
            const ReadResult read = watcher->result();
            watcher->deleteLater();
            markDocumentLoading(documentId, false);
            endLoading();
            // A newer external change (or a reload) started while this read
            // was in flight: its result supersedes ours.
            if (m_externalChangeGenerations.value(normalizedPath(documentPath))
                != generation) {
                return;
            }
            const int current = indexForPath(documentPath);
            if (current < 0 || m_documents.at(current).id != documentId) {
                return;
            }
            if (read.ok && read.mtime < m_documents.at(current).diskMtime) {
                return;
            }
            applyExternalRead(documentPath, read);
        });
        watcher->setFuture(QtConcurrent::run([documentPath]() {
            return readTextFile(documentPath);
        }));
        return;
    }
    const ReadResult read = readTextFile(documentPath);
    applyExternalRead(path, read);
}

void DocumentManager::applyExternalRead(const QString &path,
                                        const ReadResult &read) {
    const int row = indexForPath(path);
    if (row < 0) return;
    auto &document = m_documents[row];
    if (!read.ok) {
        document.missing = !QFileInfo::exists(document.path);
        notifyDocument(row, {MissingRole});
        if (row == m_activeIndex) emit activeDocumentChanged();
        return;
    }
    watchPath(document.path);
    if (read.text == document.text) {
        const bool stateChanged = document.missing;
        document.diskMtime = read.mtime;
        document.missing = false;
        if (stateChanged) {
            notifyDocument(row, {MissingRole});
            if (row == m_activeIndex) emit activeDocumentChanged();
            schedulePersist();
        }
        return;
    }
    applyRead(document, read);
    document.missing = false;
    notifyDocument(row);
    if (row == m_activeIndex) { emit activeTextChanged(); emit activeDocumentChanged(); }
    schedulePersist();
}

void DocumentManager::notifyDocument(int index, const QVector<int> &roles) {
    if (index >= 0 && index < m_documents.size()) emit dataChanged(this->index(index), this->index(index), roles);
}
