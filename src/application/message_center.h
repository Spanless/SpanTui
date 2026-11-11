#pragma once

#include <QAbstractListModel>
#include <QHash>
#include <QString>
#include <QTimer>
#include <QVariantList>
#include <QVector>
#include <QtGlobal>

class QMessageLogContext;

namespace span::application {

enum class MessageKind : quint8 {
    Info = 0,
    Success,
    Warning,
    Error,
};

class MessageCenter final : public QAbstractListModel {
    Q_OBJECT
    Q_PROPERTY(int count READ count NOTIFY countChanged FINAL)
    Q_PROPERTY(int unreadCount READ unreadCount NOTIFY unreadCountChanged FINAL)
    Q_PROPERTY(int unreadErrorCount READ unreadErrorCount NOTIFY unreadCountChanged FINAL)
    Q_PROPERTY(bool inboxOpen READ inboxOpen WRITE setInboxOpen NOTIFY inboxOpenChanged FINAL)
    Q_PROPERTY(QVariantList toasts READ toasts NOTIFY toastsChanged FINAL)

public:
    enum Role {
        KindRole = Qt::UserRole + 1,
        TitleRole,
        BodyRole,
        TimestampRole,
    };

    explicit MessageCenter(QObject *parent = nullptr);
    ~MessageCenter() override;

    static MessageCenter *instance();

    int rowCount(const QModelIndex &parent = QModelIndex()) const override;
    QVariant data(const QModelIndex &index, int role = Qt::DisplayRole) const override;
    QHash<int, QByteArray> roleNames() const override;

    int count() const;
    int unreadCount() const;
    int unreadErrorCount() const;
    bool inboxOpen() const;
    void setInboxOpen(bool open);
    QVariantList toasts() const;

    Q_INVOKABLE void post(const QString &kind,
                          const QString &body,
                          const QString &title = QString());
    void post(MessageKind kind,
              const QString &body,
              const QString &title = QString());
    void postUnhandledException(const QString &detail);
    void postFromLog(QtMsgType type,
                     const QMessageLogContext &context,
                     const QString &message);

    Q_INVOKABLE void dismissToast(qint64 id);
    Q_INVOKABLE void clear();

    void attachToQtLogging();
    bool initializePersistence(const QString &databasePath);
    void setAutoDismissMsForTesting(int milliseconds) { m_autoDismissMs = milliseconds; }

signals:
    void countChanged();
    void unreadCountChanged();
    void inboxOpenChanged();
    void toastsChanged();

private:
    struct Entry {
        qint64 id = 0;
        MessageKind kind = MessageKind::Info;
        QString title;
        QString body;
        qint64 timestampMs = 0;
        bool read = false;
        bool toastVisible = false;
    };

    // Storage writes are queued and flushed in one transaction; entry ids are
    // model-local and map to autoincrement row ids once a flush inserts them.
    struct PendingWrite {
        enum class Kind : quint8 { Insert, Remove, MarkAllRead, Clear };
        Kind kind = Kind::Insert;
        Entry entry;
        qint64 id = 0;
    };

    static MessageKind parseKind(const QString &kind);
    static QString kindName(MessageKind kind);
    QString defaultTitle(MessageKind kind) const;
    void postQueued(MessageKind kind, const QString &body, const QString &title);
    void postOnGui(MessageKind kind, const QString &body, const QString &title);
    bool loadPersistedEntries(QVector<Entry> *entries, QString *errorMessage);
    void queueWrite(PendingWrite write);
    bool flushPendingWrites(QString *errorMessage);
    bool applyPendingWrite(const PendingWrite &write, QString *errorMessage);
    bool prunePersistedEntries(QString *errorMessage);
    void flushOrDisable();
    void disablePersistence(const QString &errorMessage);
    void closePersistence();
    void rebuildUnread();
    void markAllRead();
    void dismissAllToasts();
    void hideOldestToastIfNeeded();
    int indexOfId(qint64 id) const;
    QVariantMap toastMap(const Entry &entry) const;

    QVector<Entry> m_entries;
    qint64 m_nextId = 1;
    int m_unreadCount = 0;
    int m_unreadErrorCount = 0;
    bool m_inboxOpen = false;
    int m_autoDismissMs = 5000;
    QString m_persistenceConnectionName;
    bool m_persistenceEnabled = false;
    bool m_persistenceFailureReported = false;
    bool m_posting = false;
    QVector<PendingWrite> m_pendingWrites;
    QHash<qint64, qint64> m_storageIds;
    QTimer m_flushTimer;
};

} // namespace span::application
