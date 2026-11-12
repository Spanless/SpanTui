#include "git/git_lane_scheduler.h"

#include "core/core_constants.h"
#include "core/external_tools.h"
#include "git/git_constants.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QLatin1String>
#include <QProcessEnvironment>
#include <QUuid>

#include <utility>

namespace {
constexpr qsizetype kDefaultOutputLimit = qsizetype(16) * 1024 * 1024;
constexpr qsizetype kHistoryOutputLimit = qsizetype(128) * 1024 * 1024;
constexpr int kOutputFileWatchdogMs = 500;

const QProcessEnvironment &baseGitEnvironment() {
    static const QProcessEnvironment env = []() {
        QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
        environment.insert(QStringLiteral("GIT_TERMINAL_PROMPT"), QStringLiteral("0"));
        environment.insert(QStringLiteral("GIT_EDITOR"), QStringLiteral("true"));
        environment.insert(QStringLiteral("LC_ALL"), QStringLiteral("C.UTF-8"));
        environment.insert(QStringLiteral("LANG"), QStringLiteral("C.UTF-8"));
        environment.insert(QStringLiteral("GIT_OPTIONAL_LOCKS"), QStringLiteral("0"));
        return environment;
    }();
    return env;
}
} // namespace

GitLaneScheduler::GitLaneScheduler(FailureDetail failureDetail)
    : m_failureDetail(std::move(failureDetail))
{
    for (int i = 0; i < int(Lane::Count); ++i)
        wireLane(Lane(i));
    m_outputFileWatchdog.setInterval(kOutputFileWatchdogMs);
    connect(&m_outputFileWatchdog, &QTimer::timeout, this,
            &GitLaneScheduler::checkOutputFileLimits);
}

GitLaneScheduler::~GitLaneScheduler()
{
    m_outputFileWatchdog.stop();
    for (LaneState &lane : m_lanes) {
        disconnect(&lane.process, nullptr, this, nullptr);
        lane.completion = {};
        lane.queue.clear();
        if (lane.process.state() != QProcess::NotRunning) {
            lane.process.kill();
            lane.process.waitForFinished(span::core::kProcessKillWaitMs);
        }
        lane.process.setStandardOutputFile(QString());
        removeStdoutFile(lane);
    }
}

GitLaneScheduler::OperationTraits GitLaneScheduler::operationTraits(const QString &operation)
{
    struct Row {
        QLatin1StringView name;
        OperationTraits traits;
    };
    // Operations not listed (refresh, base-content, commit, ...) take the
    // OperationTraits defaults.
    static const Row rows[] = {
        {kGitOperationHistory, {Lane::History, nullptr, true}},
        {kGitOperationHistoryParse, {Lane::History, nullptr, false}},
        {kGitOperationBranches, {Lane::History, nullptr, false}},
        {kGitOperationWorktrees, {Lane::History, nullptr, false}},
        // Discovery and refresh both read the worktree state.
        {kGitOperationDiscover, {Lane::Status, kGitOperationRefresh.data(), false}},
        // One commit view loads in steps; a newer selection supersedes
        // whichever step of the older one is running.
        {kGitOperationCommitDetails, {Lane::Diff, "commit-view", true}},
        {kGitOperationCommitFiles, {Lane::Diff, "commit-view", true}},
        {kGitOperationCommitFilesStats, {Lane::Diff, "commit-view", true}},
        {QLatin1StringView("diff-stats"), {Lane::Diff, nullptr, false}},
        {QLatin1StringView("commit-preview"), {Lane::Peek, nullptr, false}},
        {QLatin1StringView("diff-body"), {Lane::DiffBody, nullptr, false}},
        // One worktree at a time; a new round supersedes the running probe.
        {kGitOperationWorktreeStatus, {Lane::Probe, nullptr, false}},
    };
    for (const Row &row : rows) {
        if (operation == row.name) return row.traits;
    }
    return {};
}

GitLaneScheduler::Lane GitLaneScheduler::laneForOperation(const QString &operation)
{
    return operationTraits(operation).lane;
}

QString GitLaneScheduler::supersedeKey(const QString &operation)
{
    const char *kind = operationTraits(operation).kind;
    return kind ? QString::fromLatin1(kind) : operation;
}

bool GitLaneScheduler::completionOwnsBusy(const QString &operation)
{
    return operationTraits(operation).ownsBusy;
}

GitLaneScheduler::LaneState &GitLaneScheduler::laneState(Lane lane)
{
    return m_lanes[int(lane)];
}

const GitLaneScheduler::LaneState &GitLaneScheduler::laneState(Lane lane) const
{
    return m_lanes[int(lane)];
}

quint64 GitLaneScheduler::generation() const { return m_generation; }
bool GitLaneScheduler::busy() const { return m_busy; }
bool GitLaneScheduler::laneBusy(Lane lane) const { return laneState(lane).busy; }
QString GitLaneScheduler::laneOperation(Lane lane) const { return laneState(lane).operation; }

bool GitLaneScheduler::laneRunning(Lane lane) const
{
    return laneState(lane).process.state() != QProcess::NotRunning;
}

bool GitLaneScheduler::anyLaneRunning() const
{
    for (int i = 0; i < int(Lane::Count); ++i) {
        if (laneRunning(Lane(i)))
            return true;
    }
    return false;
}

bool GitLaneScheduler::refreshQueued() const
{
    for (const PendingGit &pending : laneState(Lane::Status).queue) {
        if (supersedeKey(pending.operation) == QLatin1String("refresh"))
            return true;
    }
    return false;
}

bool GitLaneScheduler::outputLimitExceeded(Lane lane) const
{
    return laneState(lane).outputLimitExceeded;
}

QString GitLaneScheduler::takeStdoutFile(Lane lane)
{
    return std::exchange(laneState(lane).stdoutFile, QString());
}

bool GitLaneScheduler::run(const QString &directory, const QStringList &arguments,
                           const QString &operation, Completion completion, bool reportBusy)
{
    if (directory.isEmpty())
        return false;
    const Lane lane = laneForOperation(operation);
    if (!laneRunning(lane) && laneState(lane).queue.isEmpty())
        return startProcess(lane, directory, arguments, operation, std::move(completion),
                            reportBusy);
    return enqueue(lane, directory, arguments, operation, std::move(completion), reportBusy);
}

void GitLaneScheduler::invalidateResults()
{
    ++m_generation;
}

bool GitLaneScheduler::cancelAll()
{
    bool anyRunning = false;
    for (LaneState &lane : m_lanes) {
        lane.queue.clear();
        lane.completion = {};
        if (lane.process.state() != QProcess::NotRunning) {
            lane.cancelRequested = true;
            lane.process.kill();
            anyRunning = true;
        }
    }
    return anyRunning;
}

void GitLaneScheduler::reset()
{
    bool busyStateChanged = false;
    for (LaneState &lane : m_lanes) {
        lane.process.setStandardOutputFile(QString());
        removeStdoutFile(lane);
        lane.queue.clear();
        busyStateChanged = busyStateChanged || lane.busy || !lane.operation.isEmpty();
        lane.busy = false;
        lane.reportBusy = false;
        lane.operation.clear();
    }
    if (busyStateChanged)
        laneBusyStateChanged();
}

void GitLaneScheduler::removeStdoutFile(LaneState &lane)
{
    if (lane.stdoutFile.isEmpty())
        return;
    QFile::remove(lane.stdoutFile);
    lane.stdoutFile.clear();
    for (const LaneState &other : m_lanes) {
        if (!other.stdoutFile.isEmpty())
            return;
    }
    m_outputFileWatchdog.stop();
}

void GitLaneScheduler::checkOutputFileLimits()
{
    bool anyStdoutFile = false;
    for (LaneState &lane : m_lanes) {
        if (lane.stdoutFile.isEmpty())
            continue;
        anyStdoutFile = true;
        if (lane.outputLimitExceeded)
            continue;
        if (QFileInfo(lane.stdoutFile).size() > lane.outputLimit) {
            lane.outputLimitExceeded = true;
            lane.process.kill();
        }
    }
    if (!anyStdoutFile)
        m_outputFileWatchdog.stop();
}

void GitLaneScheduler::wireLane(Lane id)
{
    LaneState &state = laneState(id);
    connect(&state.process, &QProcess::readyReadStandardOutput, this, [this, id]() {
        LaneState &lane = laneState(id);
        if (!lane.stdoutFile.isEmpty())
            return;
        lane.stdoutBytes += lane.process.readAllStandardOutput();
        if (lane.stdoutBytes.size() > lane.outputLimit) {
            lane.outputLimitExceeded = true;
            lane.process.kill();
        }
    });
    connect(&state.process, &QProcess::readyReadStandardError, this, [this, id]() {
        LaneState &lane = laneState(id);
        lane.stderrBytes += lane.process.readAllStandardError();
        if (lane.stderrBytes.size() > lane.outputLimit) {
            lane.outputLimitExceeded = true;
            lane.process.kill();
        }
    });
    connect(&state.process, qOverload<int, QProcess::ExitStatus>(&QProcess::finished),
            this, [this, id](int code, QProcess::ExitStatus status) {
        finishLane(id, code, status);
    });
    connect(&state.process, &QProcess::errorOccurred, this,
            [this, id](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart)
            failStart(id);
    });
}

void GitLaneScheduler::failStart(Lane id)
{
    LaneState &lane = laneState(id);
    const QString message = tr("Git executable could not be started: %1")
                                .arg(lane.process.errorString());

    // FailedToStart does not emit finished(). Drive the same ownership
    // transitions here so completions, queued work and discovery polling
    // cannot remain stranded.
    if (lane.superseded) {
        lane.superseded = false;
        lane.completion = {};
        lane.reportBusy = false;
        lane.process.setStandardOutputFile(QString());
        removeStdoutFile(lane);
        startNextQueued(id);
        return;
    }

    Completion completion = std::move(lane.completion);
    lane.completion = {};
    const bool reportBusy = lane.reportBusy;
    const QString operation = lane.runningOperation;
    lane.reportBusy = false;
    if (reportBusy)
        emit errorReported(message);
    if (completion)
        completion(false, {}, message.toUtf8());
    lane.process.setStandardOutputFile(QString());
    removeStdoutFile(lane);
    if (reportBusy && lane.busy)
        finishLaneTask(id, operation, span::core::OperationOutcome::Failed, message);
    startNextQueued(id);
}

void GitLaneScheduler::settleSupersededBusy(Lane lane)
{
    LaneState &state = laneState(lane);
    const QString operation = state.runningOperation;
    if (!state.busy || !state.reportBusy || completionOwnsBusy(operation))
        return;
    setLaneBusy(lane, false, operation);
    emitLaneProgressFinished(lane, operation, span::core::OperationOutcome::Cancelled,
                             tr("Git operation superseded by a newer request."));
}

bool GitLaneScheduler::enqueue(Lane lane, const QString &directory,
                               const QStringList &arguments, const QString &operation,
                               Completion completion, bool reportBusy)
{
    // Only the same kind is superseded; other kinds keep running and this one
    // queues: killing them would drop their follow-on loads.
    LaneState &state = laneState(lane);
    const QString key = supersedeKey(operation);
    const bool runningSameKind = laneRunning(lane) && !state.superseded
        && supersedeKey(state.runningOperation) == key;
    int waiting = -1;
    for (int i = 0; i < state.queue.size(); ++i) {
        if (supersedeKey(state.queue.at(i).operation) == key) {
            waiting = i;
            break;
        }
    }
    // A silent request never takes over a user-visible one of its kind.
    if (!reportBusy
        && ((runningSameKind && completionOwnsBusy(state.runningOperation))
            || (waiting >= 0 && completionOwnsBusy(state.queue.at(waiting).operation)))) {
        return false;
    }
    // Close the superseded request's progress before this one opens its own.
    if (runningSameKind)
        settleSupersededBusy(lane);
    if (reportBusy) {
        emit errorReported(QString());
        setLaneBusy(lane, true, operation);
    }
    PendingGit next {directory, arguments, operation, std::move(completion), reportBusy};
    if (waiting >= 0)
        state.queue[waiting] = std::move(next);
    else
        state.queue.append(std::move(next));
    if (runningSameKind || waiting >= 0)
        emit requestSuperseded(lane);
    if (runningSameKind) {
        state.superseded = true;
        state.completion = {};
        state.process.kill();
    }
    if (!laneRunning(lane))
        startNextQueued(lane);
    return true;
}

void GitLaneScheduler::startNextQueued(Lane id)
{
    LaneState &lane = laneState(id);
    while (!laneRunning(id) && !lane.queue.isEmpty()) {
        PendingGit next = lane.queue.takeFirst();
        if (startProcess(id, next.directory, next.arguments, next.operation,
                         std::move(next.completion), next.reportBusy)) {
            return;
        }
        if (next.reportBusy) {
            setLaneBusy(id, false, next.operation);
            const QString message = tr("Another Git operation is already running.");
            emit errorReported(message);
            emitLaneProgressFinished(id, next.operation, span::core::OperationOutcome::Failed,
                                     message);
        }
    }
}

void GitLaneScheduler::dropQueue(Lane id, const QString &message)
{
    const QList<PendingGit> dropped = std::exchange(laneState(id).queue, {});
    for (const PendingGit &pending : dropped) {
        if (!pending.reportBusy)
            continue;
        emit errorReported(message);
        finishLaneTask(id, pending.operation, span::core::OperationOutcome::Cancelled, message);
    }
}

bool GitLaneScheduler::startProcess(Lane lane, const QString &directory,
                                    const QStringList &arguments, const QString &operation,
                                    Completion completion, bool reportBusy)
{
    LaneState &state = laneState(lane);
    if (directory.isEmpty() || state.process.state() != QProcess::NotRunning)
        return false;
    if (reportBusy)
        emit errorReported(QString());
    const bool history = operation == QLatin1String("history");
    state.stdoutBytes.clear();
    state.stderrBytes.clear();
    state.completion = std::move(completion);
    state.cancelRequested = false;
    state.superseded = false;
    state.outputLimitExceeded = false;
    state.outputLimit = history ? kHistoryOutputLimit : kDefaultOutputLimit;
    state.processGeneration = m_generation;
    state.reportBusy = reportBusy;
    state.operation = operation;
    state.runningOperation = operation;
    state.process.setStandardOutputFile(QString());
    removeStdoutFile(state);
    QStringList args {QStringLiteral("-c"), QStringLiteral("core.quotepath=false"),
                      QStringLiteral("-c"), QStringLiteral("core.editor=true"),
                      QStringLiteral("-C"), directory};
    args.append(arguments);
    state.process.setProcessEnvironment(baseGitEnvironment());
    state.process.setProcessChannelMode(QProcess::SeparateChannels);
    if (history) {
        state.stdoutFile = QDir::temp().filePath(
            QStringLiteral("spantui-gitlog-%1")
                .arg(QUuid::createUuid().toString(QUuid::WithoutBraces)));
        state.process.setStandardOutputFile(state.stdoutFile);
        m_outputFileWatchdog.start();
    }
    // Always an absolute path (never a bare "git", which CreateProcess would
    // also look up in the opened folder); empty while Git is unavailable.
    state.process.start(span::core::toolExecutable(span::core::ExternalTool::Git), args,
                        QIODevice::ReadOnly);
    // Only once the process runs: a busyChanged listener asking for more Git
    // work must find the lane occupied and queue.
    if (reportBusy && state.process.state() != QProcess::NotRunning)
        setLaneBusy(lane, true, operation);
    return true;
}

void GitLaneScheduler::finishLane(Lane id, int exitCode, QProcess::ExitStatus status)
{
    LaneState &lane = laneState(id);
    if (lane.superseded) {
        lane.superseded = false;
        lane.stdoutBytes.clear();
        lane.stderrBytes.clear();
        lane.completion = {};
        lane.reportBusy = false;
        lane.process.setStandardOutputFile(QString());
        removeStdoutFile(lane);
        if (lane.cancelRequested) {
            lane.cancelRequested = false;
            dropQueue(id, tr("Git operation cancelled."));
            return;
        }
        startNextQueued(id);
        return;
    }
    if (lane.stdoutFile.isEmpty())
        lane.stdoutBytes += lane.process.readAllStandardOutput();
    lane.stderrBytes += lane.process.readAllStandardError();
    // A child that writes past the limit can exit before kill() lands; the
    // limit holds on what it left behind, not on whether the kill won.
    const qint64 stdoutSize = lane.stdoutFile.isEmpty() ? lane.stdoutBytes.size()
                                                        : QFileInfo(lane.stdoutFile).size();
    if (stdoutSize > lane.outputLimit || lane.stderrBytes.size() > lane.outputLimit)
        lane.outputLimitExceeded = true;
    if (lane.outputLimitExceeded) {
        lane.stderrBytes = lane.outputLimit > kDefaultOutputLimit
            ? tr("The Git history exceeds the 128 MiB safety limit.").toUtf8()
            : tr("The selected diff exceeds the 16 MiB display limit.").toUtf8();
    }
    const bool stale = lane.processGeneration != m_generation;
    const bool ok = !lane.cancelRequested && !stale && !lane.outputLimitExceeded
        && status == QProcess::NormalExit && exitCode == 0;
    Completion completion = std::move(lane.completion);
    lane.completion = {};
    const bool reportBusy = lane.reportBusy;
    lane.reportBusy = false;
    const QString operation = lane.runningOperation;
    if (reportBusy && !completionOwnsBusy(operation))
        setLaneBusy(id, false, operation);
    if (stale) {
        lane.process.setStandardOutputFile(QString());
        removeStdoutFile(lane);
        if (reportBusy) {
            finishLaneTask(id, operation, span::core::OperationOutcome::Cancelled,
                           tr("Repository changed while the operation was running."));
        }
        lane.cancelRequested = false;
        // The switch cleared the queue; what waits now is for the new path.
        startNextQueued(id);
        emit staleLaneFinished(id);
        return;
    }
    if (lane.cancelRequested) {
        lane.process.setStandardOutputFile(QString());
        removeStdoutFile(lane);
        const QString message = tr("Git operation cancelled.");
        emit errorReported(message);
        if (reportBusy)
            finishLaneTask(id, operation, span::core::OperationOutcome::Cancelled, message);
        lane.cancelRequested = false;
        dropQueue(id, message);
        return;
    }
    const QByteArray stdoutBytes = lane.stdoutBytes;
    const QByteArray stderrBytes = lane.stderrBytes;
    if (completion)
        completion(ok, stdoutBytes, stderrBytes);
    if (reportBusy && !lane.busy) {
        emitLaneProgressFinished(id, operation,
                                 ok ? span::core::OperationOutcome::Succeeded
                                    : span::core::OperationOutcome::Failed,
                                 ok ? QString() : m_failureDetail(stderrBytes));
    }
    // The completion may itself have started this lane's next process.
    startNextQueued(id);
}

void GitLaneScheduler::setLaneBusy(Lane lane, bool value, const QString &operation)
{
    LaneState &state = laneState(lane);
    if (state.busy == value && (operation.isEmpty() || operation == state.operation))
        return;
    state.busy = value;
    if (value) {
        if (!operation.isEmpty())
            state.operation = operation;
        emit taskStarted(state.operation);
    } else {
        state.operation.clear();
    }
    laneBusyStateChanged();
}

void GitLaneScheduler::laneBusyStateChanged()
{
    bool anyBusy = false;
    for (const LaneState &lane : m_lanes)
        anyBusy = anyBusy || lane.busy;
    m_busy = anyBusy;
    // Per-lane loading flags hang off this signal, so it fires for every lane
    // change, not only when the aggregate flips.
    emit busyChanged();
}

void GitLaneScheduler::finishLaneTask(Lane lane, const QString &operation,
                                      span::core::OperationOutcome outcome,
                                      const QString &detail)
{
    setLaneBusy(lane, false, operation);
    emitLaneProgressFinished(lane, operation, outcome, detail);
}

void GitLaneScheduler::emitLaneProgressFinished(Lane lane, const QString &finished,
                                                span::core::OperationOutcome outcome,
                                                const QString &detail)
{
    QString operation = finished;
    if (operation.isEmpty()) {
        switch (lane) {
        case Lane::History: operation = kGitOperationHistory; break;
        case Lane::Diff: operation = kGitOperationCommitDetails; break;
        case Lane::Status: operation = kGitOperationRefresh; break;
        default: operation = QStringLiteral("commit"); break;
        }
    }
    emit taskFinished(operation, outcome, detail);
}
