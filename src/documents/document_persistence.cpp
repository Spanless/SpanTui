#include "documents/document_manager.h"

#include <QFileInfo>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QSet>

namespace {
// Same threshold as a large open in document_manager.cpp.
constexpr qint64 kAsyncReadBytes = 1024 * 1024;
}

void DocumentManager::schedulePersist() {
    if (!m_connectionName.isEmpty()) m_persistTimer.start();
}

void DocumentManager::restore() {
    QSqlQuery query(QSqlDatabase::database(m_connectionName));
    query.prepare(QStringLiteral(
        "select path,name,cursor_position,active,"
        "disk_mtime,utf8_bom from document_tabs "
        "where ifnull(owner_project,'')=? and ifnull(owner_tile,'')=? "
        "order by sort_order"));
    query.addBindValue(m_ownerProjectPath.isEmpty() ? QStringLiteral("") : m_ownerProjectPath);
    query.addBindValue(m_ownerTileId.isEmpty() ? QStringLiteral("") : m_ownerTileId);
    if (!query.exec()) {
        emit errorOccurred(query.lastError().text());
        return;
    }
    QVector<Document> largeDocuments;
    beginResetModel();
    while (query.next()) {
        Document document;
        document.id = m_nextDocumentId++;
        document.path = query.value(0).toString();
        document.name = query.value(1).toString();
        document.cursor = query.value(2).toInt();
        document.diskMtime = query.value(4).toLongLong();
        document.utf8Bom = query.value(5).toBool();
        document.markdown = isMarkdownPath(document.path);
        if (query.value(3).toBool()) m_activeIndex = m_documents.size();
        if (QFileInfo(document.path).size() >= kAsyncReadBytes) {
            // Read like a large open, once the model is populated.
            largeDocuments.push_back(document);
        } else {
            const ReadResult disk = readTextFile(document.path);
            if (disk.ok) {
                applyRead(document, disk);
                document.missing = false;
            } else {
                document.missing = !QFileInfo::exists(document.path);
            }
        }
        m_documents.push_back(document);
        watchPath(document.path);
    }
    if (m_activeIndex < 0 && !m_documents.isEmpty()) m_activeIndex = 0;
    endResetModel();
    emit countChanged();
    emit activeDocumentChanged();
    emit activeTextChanged();
    for (const Document &document : std::as_const(largeDocuments)) {
        readDocumentAsync(document.id, document.path, tr("Opening %1").arg(document.name),
                          [this](int row, const ReadResult &read) {
                              if (row < 0) return;
                              Document &restored = m_documents[row];
                              if (read.ok) {
                                  applyRead(restored, read);
                                  restored.missing = false;
                              } else {
                                  restored.missing = !QFileInfo::exists(restored.path);
                              }
                              notifyDocument(row, {MissingRole});
                              if (row == m_activeIndex) {
                                  emit activeTextChanged();
                                  emit activeDocumentChanged();
                              }
                          });
    }
}

void DocumentManager::flush() {
    if (m_connectionName.isEmpty()) return;
    auto db = QSqlDatabase::database(m_connectionName, false);
    if (!db.isValid() || !db.isOpen()) {
        emit errorOccurred(tr("Document database is not open."));
        return;
    }
    if (!db.transaction()) {
        emit errorOccurred(tr("Could not start document save transaction."));
        return;
    }

    const QString proj = m_ownerProjectPath.isEmpty() ? QStringLiteral("") : m_ownerProjectPath;
    const QString tile = m_ownerTileId.isEmpty() ? QStringLiteral("") : m_ownerTileId;

    if (m_documents.isEmpty()) {
        QSqlQuery clear(db);
        clear.prepare(QStringLiteral(
            "delete from document_tabs where ifnull(owner_project,'')=? and ifnull(owner_tile,'')=?"));
        clear.addBindValue(proj);
        clear.addBindValue(tile);
        if (!clear.exec()) {
            db.rollback();
            emit errorOccurred(clear.lastError().text());
            return;
        }
    } else {
        QSqlQuery prune(db);
        QStringList placeholders;
        for (int i = 0; i < m_documents.size(); ++i) {
            placeholders.push_back(QStringLiteral("?"));
        }
        prune.prepare(QStringLiteral(
            "delete from document_tabs where ifnull(owner_project,'')=? and ifnull(owner_tile,'')=? and path not in (")
            + placeholders.join(QLatin1Char(',')) + QStringLiteral(")"));
        prune.addBindValue(proj);
        prune.addBindValue(tile);
        for (const auto &d : m_documents) {
            prune.addBindValue(d.path);
        }
        if (!prune.exec()) {
            db.rollback();
            emit errorOccurred(prune.lastError().text());
            return;
        }

        QSqlQuery upsert(db);
        upsert.prepare(QStringLiteral(
            "insert into document_tabs(path,name,cursor_position,"
            "sort_order,active,disk_mtime,utf8_bom,owner_project,owner_tile) "
            "values(?,?,?,?,?,?,?,?,?) "
            "on conflict(owner_project,owner_tile,path) do update set "
            "name=excluded.name, "
            "cursor_position=excluded.cursor_position, "
            "sort_order=excluded.sort_order, "
            "active=excluded.active, "
            "disk_mtime=excluded.disk_mtime, "
            "utf8_bom=excluded.utf8_bom"));

        for (int i = 0; i < m_documents.size(); ++i) {
            const auto &d = m_documents.at(i);
            upsert.bindValue(0, d.path);
            upsert.bindValue(1, d.name);
            upsert.bindValue(2, d.cursor);
            upsert.bindValue(3, i);
            upsert.bindValue(4, i == m_activeIndex);
            upsert.bindValue(5, d.diskMtime);
            upsert.bindValue(6, d.utf8Bom);
            upsert.bindValue(7, proj);
            upsert.bindValue(8, tile);
            if (!upsert.exec()) {
                db.rollback();
                emit errorOccurred(upsert.lastError().text());
                return;
            }
        }
    }

    if (!db.commit()) emit errorOccurred(db.lastError().text());
}
