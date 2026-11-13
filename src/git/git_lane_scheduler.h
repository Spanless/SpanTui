#pragma once

#include "core/operation_outcome.h"

#include <QList>
#include <QObject>
#include <QProcess>
#include <QStringList>
#include <QTimer>

#include <functional>

// Read-only Git processes on independent lanes, so a long history read never
// holds up status polling. One process per lane; a newer request of the same
// kind (supersede key) replaces the older one, running or waiting.
class GitLaneScheduler final : public QObject {
    Q_OBJECT

public:
    // Probe: the branch popup's per-worktree change check, so a slow
    // worktree never holds up this repository's own reads.
    enum class Lane : int { History = 0, Status, Diff, Peek, DiffBody, Probe, Count };
    Q_ENUM(Lane)

    using Completion = std::function<void(bool, const QByteArray &, const QByteArray &)>;
    // Failure detail for a visible operation whose completion left the
    // progress open; receives the process stderr.
    using FailureDetail = std::function<QString(const QByteArray &)>;

    explicit GitLaneScheduler(FailureDetail failureDetail);
    ~GitLaneScheduler() override;

    static Lane laneForOperation(const QString &operation);
    // Every Git process SpanTui starts: no prompts or editors, English
    // messages, no optional locks.
    static const QProcessEnvironment &environment();

    // False when nothing was started or queued: an empty directory, or a
    // silent request that would supersede a visible one of its kind.
    bool run(const QString &directory, const QStringList &arguments,
             const QString &operation, Completion completion, bool reportBusy);
    // Results of processes running now are reported through
    // staleLaneFinished instead of their completions.
    void invalidateResults();
    // True while killed processes are still unwinding.
    bool cancelAll();
    // Forgets queues, temp output and busy state; running processes are left
    // to finish.
    void reset();

    quint64 generation() const;
    bool busy() const;
    bool laneBusy(Lane lane) const;
    QString laneOperation(Lane lane) const;
    bool laneRunning(Lane lane) const;
    bool anyLaneRunning() const;
    // Silent follow-ons of the running status read should wait for it
    // instead of chaining.
    bool refreshQueued() const;
    bool outputLimitExceeded(Lane lane) const;
    // History output goes to a temp file; the caller deletes it once read.
    QString takeStdoutFile(Lane lane);

    void setLaneBusy(Lane lane, bool value, const QString &operation = {});
    void finishLaneTask(Lane lane, const QString &operation,
                        span::core::OperationOutcome outcome, const QString &detail = {});

signals:
    void busyChanged();
    void taskStarted(const QString &operation);
    void taskFinished(const QString &operation, span::core::OperationOutcome outcome,
                      const QString &detail);
    // Empty clears the owner's error.
    void errorReported(const QString &message);
    void requestSuperseded(GitLaneScheduler::Lane lane);
    void staleLaneFinished(GitLaneScheduler::Lane lane);

private:
    struct PendingGit {
        QString directory;
        QStringList arguments;
        QString operation;
        Completion completion;
        bool reportBusy = true;
    };

    struct LaneState {
        QProcess process;
        Completion completion;
        // Waiting requests in arrival order, at most one per supersede key.
        QList<PendingGit> queue;
        QByteArray stdoutBytes;
        QByteArray stderrBytes;
        QString stdoutFile;
        // The lane's busy task. A queued visible request takes it over while
        // another process still runs; runningOperation names that process.
        QString operation;
        QString runningOperation;
        bool busy = false;
        bool reportBusy = false;
        bool cancelRequested = false;
        bool superseded = false;
        bool outputLimitExceeded = false;
        qsizetype outputLimit = 16 * 1024 * 1024;
        quint64 processGeneration = 0;
    };

    struct OperationTraits {
        Lane lane = Lane::Status;
        // Supersede key; nullptr means the operation name itself.
        const char *kind = nullptr;
        // The completion, not finishLane, settles the lane's busy flag.
        bool ownsBusy = false;
    };
    static OperationTraits operationTraits(const QString &operation);
    static QString supersedeKey(const QString &operation);
    static bool completionOwnsBusy(const QString &operation);

    LaneState &laneState(Lane lane);
    const LaneState &laneState(Lane lane) const;
    void wireLane(Lane lane);
    bool startProcess(Lane lane, const QString &directory, const QStringList &arguments,
                      const QString &operation, Completion completion, bool reportBusy);
    bool enqueue(Lane lane, const QString &directory, const QStringList &arguments,
                 const QString &operation, Completion completion, bool reportBusy);
    void finishLane(Lane lane, int exitCode, QProcess::ExitStatus status);
    void failStart(Lane lane);
    void startNextQueued(Lane lane);
    void dropQueue(Lane lane, const QString &message);
    void removeStdoutFile(LaneState &lane);
    void checkOutputFileLimits();
    // Before a supersede, so busy can never leak.
    void settleSupersededBusy(Lane lane);
    void emitLaneProgressFinished(Lane lane, const QString &operation,
                                  span::core::OperationOutcome outcome, const QString &detail);
    void laneBusyStateChanged();

    FailureDetail m_failureDetail;
    LaneState m_lanes[int(Lane::Count)];
    bool m_busy = false;
    quint64 m_generation = 0;
    // History stdout goes to a file the in-memory limit cannot see; polled so
    // a huge repository cannot fill the temp volume.
    QTimer m_outputFileWatchdog;
};
