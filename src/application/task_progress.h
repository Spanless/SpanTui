#pragma once

#include <QAbstractListModel>
#include <QHash>
#include <QElapsedTimer>
#include <QString>
#include <QTimer>
#include <QVector>
#include <QtGlobal>

namespace span::application {

class TaskProgressService final : public QAbstractListModel {
    Q_OBJECT
    Q_PROPERTY(int count READ count NOTIFY countChanged FINAL)
    Q_PROPERTY(int activeCount READ activeCount NOTIFY activeCountChanged FINAL)
    // The primary entry (row 0) is the newest running task, else the newest finished.
    Q_PROPERTY(QString primaryTitle READ primaryTitle NOTIFY primaryChanged FINAL)
    Q_PROPERTY(int primaryOutcome READ primaryOutcome NOTIFY primaryChanged FINAL)
    Q_PROPERTY(bool popupOpen READ popupOpen WRITE setPopupOpen NOTIFY popupOpenChanged FINAL)

public:
    enum Outcome {
        Running = 0,
        Succeeded,
        Failed,
        Cancelled,
    };
    Q_ENUM(Outcome)

    enum Role {
        TitleRole = Qt::UserRole + 1,
        DetailRole,
        ProgressRole,
        DeterminateRole,
        OverflowRole,
        RunningRole,
        SourceRole,
        OutcomeRole,
    };

    explicit TaskProgressService(QObject *parent = nullptr);

    int rowCount(const QModelIndex &parent = QModelIndex()) const override;
    QVariant data(const QModelIndex &index, int role = Qt::DisplayRole) const override;
    QHash<int, QByteArray> roleNames() const override;

    int count() const { return m_entries.size(); }
    int activeCount() const { return m_activeCount; }
    QString primaryTitle() const { return m_primaryTitle; }
    int primaryOutcome() const { return m_primaryOutcome; }
    bool popupOpen() const { return m_popupOpen; }
    void setPopupOpen(bool open);

    // Stable source key (git, update, …). Empty source or title is ignored.
    // When a wait belongs here: docs/services/application/messages-task-progress.md.
    Q_INVOKABLE bool start(const QString &source, const QString &title);
    Q_INVOKABLE void update(const QString &source, qreal progress,
                            const QString &detail = QString());
    Q_INVOKABLE void finish(const QString &source, int outcome = Succeeded,
                            const QString &detail = QString());
    Q_INVOKABLE void dismiss(const QString &source);

    void setTimingForTesting(int revealDelayMs, int finishedLingerMs)
    {
        m_revealDelayMs = revealDelayMs;
        m_finishedLingerMs = finishedLingerMs;
    }

signals:
    void countChanged();
    void activeCountChanged();
    void primaryChanged();
    void popupOpenChanged();

private:
    struct Entry {
        QString source;
        QString title;
        QString detail;
        qreal progress = -1;
        bool running = true;
        Outcome outcome = Running;
        quint64 startedOrder = 0;
        qint64 revealAtMs = 0;
        qint64 finishedAtMs = 0;
    };

    int indexOfSource(const QString &source) const;
    Entry *pendingEntry(const QString &source);
    void emitRow(int row);
    void promotePending(const QString &source);
    void resortEntries();
    void rebuildAggregates();
    void scheduleMaintenance();
    void maintainEntries();

    QVector<Entry> m_entries;
    QHash<QString, Entry> m_pendingEntries;
    QTimer m_maintenanceTimer;
    QElapsedTimer m_clock;
    quint64 m_startedOrder = 0;
    int m_revealDelayMs = 160;
    int m_finishedLingerMs = 1600;
    int m_activeCount = 0;
    bool m_popupOpen = false;
    QString m_primaryTitle;
    int m_primaryOutcome = Running;
};

} // namespace span::application
