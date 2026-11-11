#include "documents/document_manager.h"

#include "core/core_constants.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFutureWatcher>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QStringDecoder>
#include <QUuid>
#include <QtConcurrent>

#include <algorithm>

namespace {
constexpr qint64 kMaximumTextBytes = 8 * 1024 * 1024;
constexpr qint64 kProgressReportBytes = 1024 * 1024;
constexpr int kPersistDelayMs = 250;
// Editors save in several writes; one reload after they stop.
constexpr int kExternalChangeSettleMs = 80;
}

DocumentManager::DocumentManager(QObject *parent) : QAbstractListModel(parent) {
    m_persistTimer.setSingleShot(true);
    m_persistTimer.setInterval(kPersistDelayMs);
    connect(&m_persistTimer, &QTimer::timeout, this, &DocumentManager::flush);
    connect(&m_watcher, &QFileSystemWatcher::fileChanged, this, [this](const QString &path) {
        const QString key = normalizedPath(path);
        const quint64 generation = ++m_externalChangeGenerations[key];
        QTimer::singleShot(kExternalChangeSettleMs, this, [this, path, key, generation]() {
            if (m_externalChangeGenerations.value(key) != generation) {
                return;
            }
            handleExternalChange(path);
        });
    });
    connect(this, &DocumentManager::errorOccurred, this,
            [this](const QString &message) {
                if (m_progressDepth > 0)
                    m_progressError = message.trimmed();
            });
}

bool DocumentManager::loading() const
{
    return m_progressDepth > 0 || !m_loadingDocumentIds.isEmpty();
}

bool DocumentManager::loadingAt(int index) const
{
    if (index < 0 || index >= m_documents.size())
        return false;
    return m_loadingDocumentIds.contains(m_documents.at(index).id);
}

void DocumentManager::beginLoading(const QString &title)
{
    if (m_progressDepth == 0)
        m_progressError.clear();
    ++m_progressDepth;
    emit progressStarted(title);
    emit progressChanged(-1, {});
    if (m_progressDepth == 1 || !m_loadingDocumentIds.isEmpty())
        emit loadingChanged();
}

void DocumentManager::endLoading()
{
    const bool wasLoading = m_progressDepth > 0;
    m_progressDepth = qMax(0, m_progressDepth - 1);
    if ((wasLoading && m_progressDepth == 0) || m_loadingDocumentIds.isEmpty())
        emit loadingChanged();
    if (m_progressDepth > 0)
        return;
    emit progressFinished(
        m_progressError.isEmpty()
            ? span::core::OperationOutcome::Succeeded
            : span::core::OperationOutcome::Failed,
        m_progressError);
    m_progressError.clear();
}

void DocumentManager::markDocumentLoading(qulonglong id, bool loading)
{
    const bool had = m_loadingDocumentIds.contains(id);
    if (loading == had)
        return;
    if (loading)
        m_loadingDocumentIds.insert(id);
    else
        m_loadingDocumentIds.remove(id);
    emit loadingChanged();
}

void DocumentManager::setOwnerScope(const QString &projectPath,
                                    const QString &tileId)
{
    Q_ASSERT(m_connectionName.isEmpty());
    m_ownerProjectPath = QDir::cleanPath(projectPath);
    m_ownerTileId = tileId;
}

DocumentManager::~DocumentManager() {
    flush();
    if (!m_connectionName.isEmpty()) {
        {
            auto db = QSqlDatabase::database(m_connectionName, false);
            if (db.isValid()) db.close();
        }
        QSqlDatabase::removeDatabase(m_connectionName);
    }
}

int DocumentManager::rowCount(const QModelIndex &parent) const {
    return parent.isValid() ? 0 : m_documents.size();
}

QVariant DocumentManager::data(const QModelIndex &index, int role) const {
    if (!index.isValid() || index.row() < 0 || index.row() >= m_documents.size()) return {};
    const auto &document = m_documents.at(index.row());
    switch (role) {
    case PathRole: return document.path;
    case NameRole: return document.name;
    case MissingRole: return document.missing;
    default: return {};
    }
}

QHash<int, QByteArray> DocumentManager::roleNames() const {
    return {{PathRole, "path"}, {NameRole, "name"}, {MissingRole, "missing"}};
}

bool DocumentManager::initialize(const QString &databasePath) {
    if (!m_connectionName.isEmpty()) return true;
    m_connectionName = QStringLiteral("spantui-documents-") + QUuid::createUuid().toString(QUuid::WithoutBraces);
    auto db = QSqlDatabase::addDatabase(span::core::kSqliteDriver, m_connectionName);
    db.setDatabaseName(databasePath);
    if (!db.open()) {
        // Drop the half-open connection so a later initialize() retries
        // instead of returning true without a database.
        emit errorOccurred(db.lastError().text());
        db = QSqlDatabase();
        QSqlDatabase::removeDatabase(m_connectionName);
        m_connectionName.clear();
        return false;
    }
    QSqlQuery pragma(db);
    pragma.exec(QStringLiteral("pragma journal_mode=WAL"));
    pragma.exec(span::core::kSqliteBusyTimeoutPragma);
    restore();
    return true;
}

int DocumentManager::activeIndex() const { return m_activeIndex; }
const DocumentManager::Document *DocumentManager::activeDocument() const {
    return m_activeIndex >= 0 && m_activeIndex < m_documents.size() ? &m_documents.at(m_activeIndex) : nullptr;
}
DocumentManager::Document *DocumentManager::activeDocument() {
    return m_activeIndex >= 0 && m_activeIndex < m_documents.size() ? &m_documents[m_activeIndex] : nullptr;
}
QString DocumentManager::activeName() const { const auto *d = activeDocument(); return d ? d->name : QString(); }
QString DocumentManager::activeText() const { const auto *d = activeDocument(); return d ? d->text : QString(); }
bool DocumentManager::activeMarkdown() const { const auto *d = activeDocument(); return d && d->markdown; }
bool DocumentManager::activeMissing() const { const auto *d = activeDocument(); return d && d->missing; }

int DocumentManager::indexForCanonicalPath(const QString &path) const
{
    QString canonical = QFileInfo(path).canonicalFilePath();
    if (canonical.isEmpty()) {
        canonical = QDir::cleanPath(QFileInfo(path).absoluteFilePath());
    }
    for (int i = 0; i < m_documents.size(); ++i) {
        if (QDir::cleanPath(m_documents.at(i).path)
                .compare(canonical, Qt::CaseInsensitive) == 0) {
            return i;
        }
    }
    return -1;
}

QString DocumentManager::textAt(int index) const {
    return index >= 0 && index < m_documents.size() ? m_documents.at(index).text : QString();
}
QString DocumentManager::pathAt(int index) const {
    return index >= 0 && index < m_documents.size() ? m_documents.at(index).path : QString();
}
int DocumentManager::cursorAt(int index) const {
    return index >= 0 && index < m_documents.size() ? m_documents.at(index).cursor : 0;
}
QUrl DocumentManager::activeBaseUrl() const {
    const auto *d = activeDocument();
    return d ? QUrl::fromLocalFile(QFileInfo(d->path).absolutePath() + QDir::separator()) : QUrl();
}

QString DocumentManager::normalizedPath(const QString &path) {
    QFileInfo info(path);
    const QString result = info.exists() ? info.canonicalFilePath() : info.absoluteFilePath();
    return QDir::fromNativeSeparators(QDir::cleanPath(result));
}

void DocumentManager::normalizeLineEndings(QString &text) {
    if (text.contains(QStringLiteral("\r\n")))
        text.replace(QStringLiteral("\r\n"), QStringLiteral("\n"));
    else
        text.replace(QLatin1Char('\r'), QLatin1Char('\n'));
}

bool DocumentManager::isMarkdownPath(const QString &path) {
    const QString suffix = QFileInfo(path).suffix().toLower();
    return suffix == QStringLiteral("md") || suffix == QStringLiteral("markdown")
        || suffix == QStringLiteral("mdown");
}

void DocumentManager::applyRead(Document &document, const ReadResult &read) {
    document.text = read.text;
    document.diskMtime = read.mtime;
    document.utf8Bom = read.bom;
}

DocumentManager::ReadResult DocumentManager::readTextFile(const QString &path) {
    ReadResult result;
    QFileInfo info(path);
    if (!info.exists() || !info.isFile()) { result.error = tr("File no longer exists: %1").arg(path); return result; }
    if (info.size() > kMaximumTextBytes) { result.error = tr("File is larger than 8 MiB: %1").arg(info.fileName()); return result; }
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) { result.error = file.errorString(); return result; }
    QByteArray bytes;
    bytes.reserve(int(info.size()));
    while (!file.atEnd()) {
        const QByteArray chunk = file.read(256 * 1024);
        if (chunk.isEmpty() && file.error() != QFile::NoError) {
            result.error = file.errorString();
            return result;
        }
        bytes.append(chunk);
    }
    if (bytes.contains('\0')) { result.error = tr("Binary files cannot be opened: %1").arg(info.fileName()); return result; }
    result.bom = bytes.startsWith("\xEF\xBB\xBF");
    if (result.bom) bytes.remove(0, 3);
    QStringDecoder decoder(QStringDecoder::Utf8);
    result.text = decoder.decode(bytes);
    if (decoder.hasError()) { result.text.clear(); result.error = tr("File is not valid UTF-8: %1").arg(info.fileName()); return result; }
    normalizeLineEndings(result.text);
    result.ok = true;
    result.mtime = info.lastModified().toMSecsSinceEpoch();
    return result;
}

void DocumentManager::readDocumentAsync(qulonglong documentId, const QString &path, const QString &title,
                                        std::function<void(int, const ReadResult &)> apply)
{
    beginLoading(title);
    markDocumentLoading(documentId, true);
    // An external change of the path while the read runs starts its own
    // read; that newer result wins and this one is dropped.
    const quint64 generation = m_externalChangeGenerations.value(normalizedPath(path));
    auto *watcher = new QFutureWatcher<ReadResult>(this);
    connect(watcher, &QFutureWatcher<ReadResult>::finished, this,
            [this, watcher, documentId, path, generation, apply = std::move(apply)]() {
                const ReadResult read = watcher->result();
                watcher->deleteLater();
                int row = -1;
                for (int i = 0; i < m_documents.size(); ++i) {
                    if (m_documents.at(i).id == documentId) {
                        row = i;
                        break;
                    }
                }
                if (m_externalChangeGenerations.value(normalizedPath(path)) != generation) row = -1;
                apply(row, read);
                markDocumentLoading(documentId, false);
                endLoading();
            });
    watcher->setFuture(QtConcurrent::run([path]() { return readTextFile(path); }));
}

int DocumentManager::indexForPath(const QString &path) const {
    const QString normalized = normalizedPath(path);
    for (int i = 0; i < m_documents.size(); ++i) {
        if (QString::compare(m_documents.at(i).path, normalized, Qt::CaseInsensitive) == 0) return i;
    }
    return -1;
}

bool DocumentManager::openFile(const QString &path) {
    const QString normalized = normalizedPath(path);
    const int existing = indexForPath(normalized);
    if (existing >= 0) {
        handleExternalChange(m_documents.at(existing).path);
        activate(existing);
        return true;
    }
    if (QFileInfo(normalized).size() >= kProgressReportBytes) {
        const QString title = tr("Opening %1").arg(QFileInfo(normalized).fileName());
        const int row = m_documents.size();
        const qulonglong documentId = m_nextDocumentId++;
        beginInsertRows({}, row, row);
        Document document;
        document.id = documentId;
        document.path = normalized;
        document.name = QFileInfo(normalized).fileName();
        document.markdown = isMarkdownPath(normalized);
        m_documents.push_back(document);
        endInsertRows();
        emit countChanged();
        activate(row);
        readDocumentAsync(documentId, normalized, title, [this, normalized](int row, const ReadResult &read) {
            if (row < 0) {
                m_pendingOpenLocations.remove(normalized);
                return;
            }
            if (!read.ok) {
                m_pendingOpenLocations.remove(normalized);
                emit errorOccurred(read.error);
                closeDocument(row);
                return;
            }
            Document &opened = m_documents[row];
            applyRead(opened, read);
            const auto location = m_pendingOpenLocations.take(normalized);
            if (location.first > 0) {
                int position = 0;
                for (int line = 1; line < location.first; ++line) {
                    const int next = opened.text.indexOf(QLatin1Char('\n'), position);
                    if (next < 0) {
                        position = opened.text.size();
                        break;
                    }
                    position = next + 1;
                }
                const int lineEnd = opened.text.indexOf(QLatin1Char('\n'), position);
                opened.cursor = std::min(
                    lineEnd < 0 ? int(opened.text.size()) : lineEnd,
                    position + std::max(0, location.second - 1));
            }
            watchPath(normalized);
            // The preview rebuilds on activeDocumentChanged, so announce both
            // once the text arrives, as restore does.
            if (row == m_activeIndex) {
                emit activeTextChanged();
                emit activeDocumentChanged();
            }
            schedulePersist();
        });
        return true;
    }
    const ReadResult read = readTextFile(normalized);
    if (!read.ok) { emit errorOccurred(read.error); return false; }
    const int row = m_documents.size();
    beginInsertRows({}, row, row);
    Document document;
    document.id = m_nextDocumentId++;
    document.path = normalized;
    document.name = QFileInfo(normalized).fileName();
    applyRead(document, read);
    document.markdown = isMarkdownPath(normalized);
    m_documents.push_back(document);
    endInsertRows();
    emit countChanged();
    watchPath(normalized);
    activate(row);
    schedulePersist();
    return true;
}

bool DocumentManager::openFileAt(const QString &path, int oneBasedLine, int oneBasedColumn) {
    const QString normalized = normalizedPath(path);
    if (indexForPath(normalized) < 0
        && QFileInfo(normalized).size() >= kProgressReportBytes) {
        m_pendingOpenLocations.insert(
            normalized, {std::max(1, oneBasedLine),
                         std::max(1, oneBasedColumn)});
        if (!openFile(normalized)) {
            m_pendingOpenLocations.remove(normalized);
            return false;
        }
        return true;
    }
    if (!openFile(path)) return false;
    Document *document = activeDocument(); if (!document) return false;
    int position = 0;
    const int targetLine = std::max(1, oneBasedLine);
    for (int line = 1; line < targetLine; ++line) {
        const int next = document->text.indexOf(QLatin1Char('\n'), position);
        if (next < 0) { position = document->text.size(); break; }
        position = next + 1;
    }
    const int lineEnd = document->text.indexOf(QLatin1Char('\n'), position);
    position = std::min(lineEnd < 0 ? int(document->text.size()) : lineEnd,
                        position + std::max(0, oneBasedColumn - 1));
    document->cursor = position;
    return true;
}

void DocumentManager::activate(int index) {
    if (index < 0 || index >= m_documents.size() || index == m_activeIndex) return;
    m_activeIndex = index;
    emit activeDocumentChanged();
    emit activeTextChanged();
    emit documentActivated();
    schedulePersist();
}

void DocumentManager::setDocumentCursor(int index, int position) {
    if (index < 0 || index >= m_documents.size()) return;
    Document &document = m_documents[index];
    document.cursor = std::clamp(position, 0, int(document.text.size()));
    schedulePersist();
}

bool DocumentManager::closeDocument(int index) {
    if (index < 0 || index >= m_documents.size()) return false;
    const QString path = m_documents.at(index).path;
    const qulonglong documentId = m_documents.at(index).id;
    markDocumentLoading(documentId, false);
    beginRemoveRows({}, index, index);
    m_documents.removeAt(index);
    endRemoveRows();
    if (m_watcher.files().contains(path) && indexForPath(path) < 0) m_watcher.removePath(path);
    if (m_documents.isEmpty()) m_activeIndex = -1;
    else if (index < m_activeIndex) --m_activeIndex;
    else if (index == m_activeIndex) m_activeIndex = std::min(index, int(m_documents.size()) - 1);
    emit countChanged();
    emit activeDocumentChanged();
    emit activeTextChanged();
    if (m_activeIndex >= 0) emit documentActivated();
    schedulePersist();
    return true;
}

void DocumentManager::updatePath(const QString &oldPath, const QString &newPath) {
    const QString oldNormalized = normalizedPath(oldPath);
    const QString newNormalized = normalizedPath(newPath);
    bool changed = false;
    for (int row = 0; row < m_documents.size(); ++row) {
        const QString current = m_documents.at(row).path;
        if (QString::compare(current, oldNormalized, Qt::CaseInsensitive) != 0 &&
            !current.startsWith(oldNormalized + QLatin1Char('/'), Qt::CaseInsensitive)) continue;
        m_watcher.removePath(current);
        const QString tail = current.mid(oldNormalized.size());
        m_documents[row].path = QDir::cleanPath(newNormalized + tail);
        m_documents[row].name = QFileInfo(m_documents[row].path).fileName();
        m_documents[row].markdown = isMarkdownPath(m_documents[row].path);
        watchPath(m_documents[row].path);
        notifyDocument(row);
        changed = true;
    }
    if (!changed) return;
    emit activeDocumentChanged();
    schedulePersist();
}

void DocumentManager::markPathDeleted(const QString &path) {
    const QString normalized = normalizedPath(path);
    for (int i = 0; i < m_documents.size(); ++i) {
        if (QString::compare(m_documents.at(i).path, normalized, Qt::CaseInsensitive) == 0 ||
            m_documents.at(i).path.startsWith(normalized + QLatin1Char('/'), Qt::CaseInsensitive)) {
            m_documents[i].missing = true;
            notifyDocument(i, {MissingRole});
            if (i == m_activeIndex) emit activeDocumentChanged();
        }
    }
    schedulePersist();
}

void DocumentManager::refreshPath(const QString &path) {
    const QString normalized = normalizedPath(path);
    QStringList affected;
    for (const auto &document : m_documents) {
        if (QString::compare(document.path, normalized, Qt::CaseInsensitive) == 0 ||
            document.path.startsWith(normalized + QLatin1Char('/'), Qt::CaseInsensitive)) {
            affected.push_back(document.path);
        }
    }
    for (const QString &documentPath : affected) handleExternalChange(documentPath);
}
