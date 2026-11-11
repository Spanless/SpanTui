#include "application/message_center.h"

#include "core/core_constants.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDebug>
#include <QMessageLogContext>
#include <QMetaObject>
#include <QMutexLocker>
#include <QRecursiveMutex>
#include <QScopedValueRollback>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QThread>
#include <QTimer>
#include <QVariantMap>

#include <algorithm>
#include <utility>

namespace span::application {
namespace {

// The log handler runs on whichever thread logs. The lock spans reading the
// instance and queueing onto it, so the GUI thread cannot destroy the center
// in between. Recursive: a message raised while queueing re-enters here.
QRecursiveMutex g_instanceMutex;
MessageCenter *g_instance = nullptr;
QtMessageHandler g_previousHandler = nullptr;
bool g_loggingAttached = false;

constexpr int kMaxEntries = 200;
constexpr int kMaxToasts = 3;
constexpr int kDedupWindowMs = 2000;
constexpr int kFlushDelayMs = 250;

void loggingHandler(QtMsgType type,
                    const QMessageLogContext &context,
                    const QString &message)
{
    if (g_previousHandler)
        g_previousHandler(type, context, message);
    const QMutexLocker lock(&g_instanceMutex);
    if (g_instance)
        g_instance->postFromLog(type, context, message);
}

} // namespace

MessageCenter::MessageCenter(QObject *parent)
    : QAbstractListModel(parent)
{
    {
        const QMutexLocker lock(&g_instanceMutex);
        if (!g_instance)
            g_instance = this;
    }
    m_flushTimer.setSingleShot(true);
    m_flushTimer.setInterval(kFlushDelayMs);
    connect(&m_flushTimer, &QTimer::timeout, this,
            &MessageCenter::flushOrDisable);
}

MessageCenter::~MessageCenter()
{
    {
        const QMutexLocker lock(&g_instanceMutex);
        if (g_instance == this)
            g_instance = nullptr;
    }
    closePersistence();
}

MessageCenter *MessageCenter::instance()
{
    const QMutexLocker lock(&g_instanceMutex);
    return g_instance;
}

int MessageCenter::rowCount(const QModelIndex &parent) const
{
    if (parent.isValid())
        return 0;
    return m_entries.size();
}

QVariant MessageCenter::data(const QModelIndex &index, int role) const
{
    if (!index.isValid() || index.row() < 0 || index.row() >= m_entries.size())
        return {};
    const Entry &entry = m_entries.at(index.row());
    switch (role) {
    case KindRole:
        return kindName(entry.kind);
    case TitleRole:
        return entry.title;
    case BodyRole:
        return entry.body;
    case TimestampRole:
        return entry.timestampMs;
    default:
        return {};
    }
}

QHash<int, QByteArray> MessageCenter::roleNames() const
{
    return {
        {KindRole, QByteArrayLiteral("kind")},
        {TitleRole, QByteArrayLiteral("title")},
        {BodyRole, QByteArrayLiteral("body")},
        {TimestampRole, QByteArrayLiteral("timestamp")},
    };
}

int MessageCenter::count() const
{
    return m_entries.size();
}

int MessageCenter::unreadCount() const
{
    return m_unreadCount;
}

int MessageCenter::unreadErrorCount() const
{
    return m_unreadErrorCount;
}

bool MessageCenter::inboxOpen() const
{
    return m_inboxOpen;
}

void MessageCenter::setInboxOpen(bool open)
{
    if (m_inboxOpen == open)
        return;
    m_inboxOpen = open;
    emit inboxOpenChanged();
    if (open) {
        markAllRead();
        dismissAllToasts();
    }
}

QVariantList MessageCenter::toasts() const
{
    QVariantList list;
    for (const Entry &entry : m_entries) {
        if (entry.toastVisible)
            list.push_back(toastMap(entry));
    }
    return list;
}

void MessageCenter::post(const QString &kind,
                         const QString &body,
                         const QString &title)
{
    post(parseKind(kind), body, title);
}

void MessageCenter::post(MessageKind kind,
                         const QString &body,
                         const QString &title)
{
    const QString trimmedBody = body.trimmed();
    if (trimmedBody.isEmpty())
        return;
    // A post issued while another is mutating the model (e.g. a delegate
    // warning raised during rowsInserted) is deferred, never dropped.
    if (QThread::currentThread() != thread() || m_posting) {
        postQueued(kind, trimmedBody, title);
        return;
    }
    postOnGui(kind, trimmedBody, title);
}

void MessageCenter::postQueued(MessageKind kind,
                               const QString &body,
                               const QString &title)
{
    QMetaObject::invokeMethod(this, [this, kind, body, title]() {
        post(kind, body, title);
    }, Qt::QueuedConnection);
}

void MessageCenter::postUnhandledException(const QString &detail)
{
    const QString body = detail.trimmed().isEmpty()
        ? tr("An unknown exception was caught in the event loop.")
        : detail.trimmed();
    post(MessageKind::Error, body, tr("Unhandled exception"));
}

void MessageCenter::postFromLog(QtMsgType type,
                                const QMessageLogContext &,
                                const QString &message)
{
    if (type != QtWarningMsg && type != QtCriticalMsg)
        return;
    const MessageKind kind = type == QtCriticalMsg
        ? MessageKind::Error
        : MessageKind::Warning;
    // Diagnostics always enter the model through the event loop: a warning
    // emitted by the model's own consumers must not re-enter it synchronously.
    const QString body = message.trimmed();
    if (!body.isEmpty())
        postQueued(kind, body, QString());
}

void MessageCenter::dismissToast(qint64 id)
{
    const int row = indexOfId(id);
    if (row < 0)
        return;
    if (!m_entries[row].toastVisible)
        return;
    m_entries[row].toastVisible = false;
    emit toastsChanged();
}

void MessageCenter::dismissAllToasts()
{
    bool changed = false;
    for (int i = 0; i < m_entries.size(); ++i) {
        if (!m_entries[i].toastVisible)
            continue;
        m_entries[i].toastVisible = false;
        changed = true;
    }
    if (changed)
        emit toastsChanged();
}

void MessageCenter::markAllRead()
{
    if (m_unreadCount == 0)
        return;
    for (Entry &entry : m_entries)
        entry.read = true;
    m_unreadCount = 0;
    m_unreadErrorCount = 0;
    emit unreadCountChanged();

    queueWrite({PendingWrite::Kind::MarkAllRead, {}, 0});
}

void MessageCenter::clear()
{
    if (m_entries.isEmpty())
        return;
    beginResetModel();
    m_entries.clear();
    endResetModel();
    m_unreadCount = 0;
    m_unreadErrorCount = 0;
    emit countChanged();
    emit unreadCountChanged();
    emit toastsChanged();

    queueWrite({PendingWrite::Kind::Clear, {}, 0});
}

void MessageCenter::attachToQtLogging()
{
    if (g_loggingAttached)
        return;
    g_loggingAttached = true;
    g_previousHandler = qInstallMessageHandler(&loggingHandler);
}

bool MessageCenter::initializePersistence(const QString &databasePath)
{
    if (!m_persistenceConnectionName.isEmpty())
        return m_persistenceEnabled;
    if (databasePath.trimmed().isEmpty())
        return false;

    m_persistenceConnectionName =
        QStringLiteral("message_center_%1")
            .arg(static_cast<qulonglong>(reinterpret_cast<quintptr>(this)),
                 0, 16);
    {
        QSqlDatabase database = QSqlDatabase::addDatabase(span::core::kSqliteDriver, m_persistenceConnectionName);
        database.setDatabaseName(databasePath);
        if (!database.open()) {
            disablePersistence(database.lastError().text());
            return false;
        }
        QSqlQuery pragma(database);
        if (!pragma.exec(span::core::kSqliteBusyTimeoutPragma)) {
            disablePersistence(pragma.lastError().text());
            return false;
        }
    }

    QVector<Entry> storedEntries;
    QString errorMessage;
    if (!loadPersistedEntries(&storedEntries, &errorMessage)) {
        disablePersistence(errorMessage);
        return false;
    }

    // Entries posted before storage opened keep their ids (their toast
    // timers reference them) and are written oldest first.
    m_persistenceEnabled = true;
    for (int i = m_entries.size() - 1; i >= 0; --i)
        queueWrite({PendingWrite::Kind::Insert, m_entries.at(i), 0});

    QVector<Entry> combinedEntries = m_entries;
    combinedEntries += storedEntries;
    if (combinedEntries.size() > kMaxEntries) {
        for (int i = kMaxEntries; i < combinedEntries.size(); ++i)
            m_storageIds.remove(combinedEntries.at(i).id);
        combinedEntries.resize(kMaxEntries);
    }

    beginResetModel();
    m_entries = std::move(combinedEntries);
    endResetModel();

    rebuildUnread();
    emit countChanged();
    emit toastsChanged();

    m_flushTimer.stop();
    if (!flushPendingWrites(&errorMessage)) {
        disablePersistence(errorMessage);
        return false;
    }
    return true;
}

bool MessageCenter::loadPersistedEntries(QVector<Entry> *entries,
                                         QString *errorMessage)
{
    QSqlQuery query(
        QSqlDatabase::database(m_persistenceConnectionName, false));
    if (!query.exec(QStringLiteral(
            "select id, kind, title, body, created_at, read "
            "from messages order by id desc limit 200"))) {
        *errorMessage = query.lastError().text();
        return false;
    }

    entries->clear();
    entries->reserve(kMaxEntries);
    while (query.next()) {
        const QString body = query.value(3).toString().trimmed();
        if (body.isEmpty())
            continue;
        Entry entry;
        entry.id = m_nextId++;
        m_storageIds.insert(entry.id, query.value(0).toLongLong());
        entry.kind = parseKind(query.value(1).toString());
        entry.title = query.value(2).toString();
        if (entry.title.trimmed().isEmpty())
            entry.title = defaultTitle(entry.kind);
        entry.body = body;
        entry.timestampMs = query.value(4).toLongLong();
        entry.read = query.value(5).toBool();
        entry.toastVisible = false;
        entries->push_back(std::move(entry));
    }
    return true;
}

void MessageCenter::queueWrite(PendingWrite write)
{
    if (!m_persistenceEnabled)
        return;
    m_pendingWrites.push_back(std::move(write));
    if (!m_flushTimer.isActive())
        m_flushTimer.start();
}

bool MessageCenter::flushPendingWrites(QString *errorMessage)
{
    if (!m_persistenceEnabled) {
        m_pendingWrites.clear();
        return true;
    }
    if (m_pendingWrites.isEmpty())
        return true;
    QSqlDatabase database =
        QSqlDatabase::database(m_persistenceConnectionName, false);
    if (!database.transaction()) {
        *errorMessage = database.lastError().text();
        return false;
    }
    const QVector<PendingWrite> writes = std::exchange(m_pendingWrites, {});
    const QHash<qint64, qint64> storageIds = m_storageIds;
    for (const PendingWrite &write : writes) {
        if (!applyPendingWrite(write, errorMessage)) {
            database.rollback();
            m_storageIds = storageIds;
            return false;
        }
    }
    if (!prunePersistedEntries(errorMessage) || !database.commit()) {
        if (errorMessage->isEmpty())
            *errorMessage = database.lastError().text();
        database.rollback();
        m_storageIds = storageIds;
        return false;
    }
    return true;
}

bool MessageCenter::applyPendingWrite(const PendingWrite &write,
                                      QString *errorMessage)
{
    QSqlQuery query(
        QSqlDatabase::database(m_persistenceConnectionName, false));
    switch (write.kind) {
    case PendingWrite::Kind::Insert:
        query.prepare(QStringLiteral(
            "insert into messages(kind, title, body, created_at, read) "
            "values(?, ?, ?, ?, ?)"));
        query.addBindValue(kindName(write.entry.kind));
        query.addBindValue(write.entry.title);
        query.addBindValue(write.entry.body);
        query.addBindValue(write.entry.timestampMs);
        query.addBindValue(write.entry.read ? 1 : 0);
        if (!query.exec())
            break;
        m_storageIds.insert(write.entry.id, query.lastInsertId().toLongLong());
        return true;
    case PendingWrite::Kind::Remove: {
        const qint64 storageId = m_storageIds.take(write.id);
        if (storageId <= 0)
            return true;
        query.prepare(QStringLiteral("delete from messages where id = ?"));
        query.addBindValue(storageId);
        if (query.exec())
            return true;
        break;
    }
    case PendingWrite::Kind::MarkAllRead:
        if (query.exec(QStringLiteral("update messages set read = 1")))
            return true;
        break;
    case PendingWrite::Kind::Clear:
        m_storageIds.clear();
        if (query.exec(QStringLiteral("delete from messages")))
            return true;
        break;
    }
    *errorMessage = query.lastError().text();
    return false;
}

bool MessageCenter::prunePersistedEntries(QString *errorMessage)
{
    QSqlQuery query(
        QSqlDatabase::database(m_persistenceConnectionName, false));
    if (query.exec(QStringLiteral(
            "delete from messages where id not in "
            "(select id from messages order by id desc limit 200)"))) {
        return true;
    }
    *errorMessage = query.lastError().text();
    return false;
}

void MessageCenter::flushOrDisable()
{
    QString errorMessage;
    if (!flushPendingWrites(&errorMessage))
        disablePersistence(errorMessage);
}

void MessageCenter::disablePersistence(const QString &errorMessage)
{
    m_persistenceEnabled = false;
    m_pendingWrites.clear();
    m_flushTimer.stop();
    if (m_persistenceFailureReported)
        return;
    m_persistenceFailureReported = true;
    qWarning().noquote()
        << tr("Message persistence is unavailable: %1")
               .arg(errorMessage.trimmed());
}

void MessageCenter::closePersistence()
{
    if (m_persistenceConnectionName.isEmpty())
        return;
    m_flushTimer.stop();
    flushOrDisable();
    {
        QSqlDatabase database = QSqlDatabase::database(
            m_persistenceConnectionName, false);
        if (database.isOpen())
            database.close();
    }
    QSqlDatabase::removeDatabase(m_persistenceConnectionName);
    m_persistenceConnectionName.clear();
    m_persistenceEnabled = false;
    m_storageIds.clear();
}

MessageKind MessageCenter::parseKind(const QString &kind)
{
    const QString normalized = kind.trimmed().toLower();
    if (normalized == QLatin1String("error"))
        return MessageKind::Error;
    if (normalized == QLatin1String("warning"))
        return MessageKind::Warning;
    if (normalized == QLatin1String("success"))
        return MessageKind::Success;
    return MessageKind::Info;
}

QString MessageCenter::kindName(MessageKind kind)
{
    switch (kind) {
    case MessageKind::Error:
        return QStringLiteral("error");
    case MessageKind::Warning:
        return QStringLiteral("warning");
    case MessageKind::Success:
        return QStringLiteral("success");
    case MessageKind::Info:
        return QStringLiteral("info");
    }
    return QStringLiteral("info");
}

QString MessageCenter::defaultTitle(MessageKind kind) const
{
    switch (kind) {
    case MessageKind::Error:
        return tr("Error");
    case MessageKind::Warning:
        return tr("Warning");
    case MessageKind::Success:
        return tr("Done");
    case MessageKind::Info:
        return tr("Notice");
    }
    return tr("Notice");
}

void MessageCenter::postOnGui(MessageKind kind,
                              const QString &body,
                              const QString &title)
{
    const QScopedValueRollback<bool> posting(m_posting, true);
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    const QString resolvedTitle = title.trimmed().isEmpty()
        ? defaultTitle(kind)
        : title.trimmed();

    if ((kind == MessageKind::Info || kind == MessageKind::Success)
        && !m_entries.isEmpty()) {
        const Entry &newest = m_entries.constFirst();
        if (newest.kind == kind && newest.body == body
            && now - newest.timestampMs <= kDedupWindowMs) {
            if (!newest.toastVisible && !m_inboxOpen) {
                m_entries[0].toastVisible = true;
                const qint64 id = m_entries.constFirst().id;
                hideOldestToastIfNeeded();
                emit toastsChanged();
                QTimer::singleShot(m_autoDismissMs, this, [this, id]() {
                    dismissToast(id);
                });
            }
            return;
        }
    }

    Entry entry;
    entry.id = m_nextId++;
    entry.kind = kind;
    entry.title = resolvedTitle;
    entry.body = body;
    entry.timestampMs = now;
    entry.read = m_inboxOpen;
    entry.toastVisible = !m_inboxOpen;

    beginInsertRows(QModelIndex(), 0, 0);
    m_entries.prepend(std::move(entry));
    endInsertRows();

    const qint64 postedId = m_entries.constFirst().id;
    queueWrite({PendingWrite::Kind::Insert, m_entries.constFirst(), 0});

    if (m_entries.size() > kMaxEntries) {
        const qint64 removedId = m_entries.constLast().id;
        beginRemoveRows(QModelIndex(), m_entries.size() - 1,
                        m_entries.size() - 1);
        m_entries.removeLast();
        endRemoveRows();
        queueWrite({PendingWrite::Kind::Remove, {}, removedId});
    }

    if (!m_inboxOpen) {
        hideOldestToastIfNeeded();
        QTimer::singleShot(m_autoDismissMs, this, [this, postedId]() {
            dismissToast(postedId);
        });
    }

    rebuildUnread();
    emit countChanged();
    emit toastsChanged();
}

void MessageCenter::rebuildUnread()
{
    int unread = 0;
    int unreadErrors = 0;
    for (const Entry &entry : m_entries) {
        if (entry.read)
            continue;
        ++unread;
        if (entry.kind == MessageKind::Error)
            ++unreadErrors;
    }
    if (unread == m_unreadCount && unreadErrors == m_unreadErrorCount)
        return;
    m_unreadCount = unread;
    m_unreadErrorCount = unreadErrors;
    emit unreadCountChanged();
}

void MessageCenter::hideOldestToastIfNeeded()
{
    int visible = 0;
    for (const Entry &entry : m_entries) {
        if (entry.toastVisible)
            ++visible;
    }
    if (visible <= kMaxToasts)
        return;
    for (int i = m_entries.size() - 1; i >= 0; --i) {
        if (!m_entries[i].toastVisible)
            continue;
        m_entries[i].toastVisible = false;
        return;
    }
}

int MessageCenter::indexOfId(qint64 id) const
{
    for (int i = 0; i < m_entries.size(); ++i) {
        if (m_entries[i].id == id)
            return i;
    }
    return -1;
}

QVariantMap MessageCenter::toastMap(const Entry &entry) const
{
    QVariantMap map;
    map.insert(QStringLiteral("id"), entry.id);
    map.insert(QStringLiteral("kind"), kindName(entry.kind));
    map.insert(QStringLiteral("title"), entry.title);
    map.insert(QStringLiteral("body"), entry.body);
    return map;
}

} // namespace span::application
