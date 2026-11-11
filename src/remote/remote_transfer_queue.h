#pragma once

#include "core/operation_outcome.h"

#include <QHash>
#include <QObject>
#include <QQueue>
#include <QStringList>

class QProcess;

namespace span::remote {

// Upload/download jobs, one `sftp -b -` process at a time. sftp buffers its
// output when it is not attached to a console, so it is never driven
// interactively: each job writes its whole batch, closes stdin and waits for
// the exit status.
class RemoteTransferQueue final : public QObject {
    Q_OBJECT

public:
    enum class Direction {
        Upload,
        Download,
    };
    Q_ENUM(Direction)

    explicit RemoteTransferQueue(QObject *parent = nullptr);
    ~RemoteTransferQueue() override;

    // Applies to jobs enqueued from now on: queued and running jobs keep the
    // launch, and so the host, they were enqueued with.
    void setLaunch(const QString &program, const QStringList &arguments,
                   const QHash<QString, QString> &environment);
    // Upload: local files/folders into the remote directory `destination`.
    // Download: remote paths into the local directory `destination`.
    void enqueue(Direction direction, const QStringList &sources,
                 const QString &destination);
    bool busy() const { return m_process != nullptr; }

    // The sftp batch for one job; exposed for tests.
    static QByteArray batchScript(Direction direction, const QStringList &sources,
                                  const QString &destination);

signals:
    void progressStarted(const QString &title);
    void progressChanged(qreal fraction, const QString &detail);
    void progressFinished(span::core::OperationOutcome outcome, const QString &detail);
    void transferFinished(span::remote::RemoteTransferQueue::Direction direction,
                          const QString &destination, bool succeeded,
                          const QString &message);

private:
    struct Launch {
        QString program;
        QStringList arguments;
        QHash<QString, QString> environment;
    };
    struct Job {
        Direction direction = Direction::Upload;
        QStringList sources;
        QString destination;
        Launch launch;
    };

    void startNext();
    void finishCurrent(bool succeeded, const QString &message);

    Launch m_launch;
    QQueue<Job> m_jobs;
    Job m_current;
    QProcess *m_process = nullptr;
    QByteArray m_output;
};

} // namespace span::remote
