#include "git/git_lane_scheduler.h"
#include "core/external_tools.h"
#include "git_test_support.h"

#include <QDir>
#include <QFile>
#include <QScopeGuard>
#include <QTemporaryDir>
#include <QTest>

#include <algorithm>

using namespace span::tests::git;
using span::core::OperationOutcome;
using Lane = GitLaneScheduler::Lane;

namespace {

// A Git invocation that stays running until the file `gate` exists, so the
// scheduler sees a busy lane deterministically and the test releases it as
// soon as it has looked. Git for Windows runs `!` aliases in its sh. The wait
// is capped so that a sh orphaned by a killed git still exits.
QStringList blocking(const QString &gate)
{
    return {QStringLiteral("-c"),
            QStringLiteral("alias.span-wait=!i=0; while [ ! -e '%1' ] && [ $i -lt 300 ]; do "
                           "sleep 0.02; i=$((i+1)); done; echo waited")
                .arg(gate),
            QStringLiteral("span-wait")};
}

void open(const QString &gate)
{
    QFile file(gate);
    QVERIFY(file.open(QIODevice::WriteOnly));
}

QStringList quick()
{
    return {QStringLiteral("rev-parse"), QStringLiteral("--is-inside-work-tree")};
}

QString failureDetail(const QByteArray &stderrBytes)
{
    return QStringLiteral("detail:") + QString::fromUtf8(stderrBytes).trimmed();
}

// Contents of `path`; empty when it cannot be read.
QByteArray readFile(const QString &path)
{
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
}

struct Result {
    bool called = false;
    bool ok = false;
    QByteArray out;
    QByteArray err;
};

struct Finished {
    QString operation;
    OperationOutcome outcome;
    QString detail;
};

// Everything the scheduler reports, in emission order.
struct Recorder {
    QStringList order;
    QStringList started;
    QList<Finished> finished;
    QList<int> superseded;
    QList<int> stale;
    QStringList errors;
    int busyChanges = 0;

    void attach(GitLaneScheduler &scheduler)
    {
        QObject::connect(&scheduler, &GitLaneScheduler::taskStarted, &scheduler,
                         [this](const QString &operation) { started.append(operation); });
        QObject::connect(&scheduler, &GitLaneScheduler::taskFinished, &scheduler,
                         [this](const QString &operation, OperationOutcome outcome,
                                const QString &detail) {
                             finished.append({operation, outcome, detail});
                         });
        QObject::connect(&scheduler, &GitLaneScheduler::requestSuperseded, &scheduler,
                         [this](Lane lane) { superseded.append(int(lane)); });
        QObject::connect(&scheduler, &GitLaneScheduler::staleLaneFinished, &scheduler,
                         [this](Lane lane) { stale.append(int(lane)); });
        QObject::connect(&scheduler, &GitLaneScheduler::errorReported, &scheduler,
                         [this](const QString &message) { errors.append(message); });
        QObject::connect(&scheduler, &GitLaneScheduler::busyChanged, &scheduler,
                         [this]() { ++busyChanges; });
    }

    // Started operations that never finished. A takeover may announce a
    // running operation again, so a name counts as closed once any
    // taskFinished carries it.
    QStringList unfinished() const
    {
        QStringList open;
        for (const QString &operation : started) {
            const bool closed = std::any_of(finished.cbegin(), finished.cend(),
                                            [&](const Finished &done) { return done.operation == operation; });
            if (!closed) open.append(operation);
        }
        return open;
    }

    GitLaneScheduler::Completion capture(const QString &name, Result &result)
    {
        return [this, name, &result](bool ok, const QByteArray &out, const QByteArray &err) {
            order.append(name);
            result = {true, ok, out, err};
        };
    }
};

} // namespace

class GitLaneSchedulerTest final : public QObject {
    Q_OBJECT

private slots:
    void initTestCase();
    void cleanup();
    void routesOperationsToLanes_data();
    void routesOperationsToLanes();
    void rejectsEmptyDirectory();
    void reportsVisibleCompletion();
    void reportsFailureDetail();
    void sameKindSupersedesRunningRequest();
    void sameKindReplacesWaitingRequest();
    void otherKindsQueueInArrivalOrder();
    void lanesRunIndependently();
    void busyChangedFollowsEveryLane();
    void queuedVisibleRequestKeepsRunningProgress();
    void progressNamesItsOwnOperation();
    void silentRequestNeverTakesOverVisibleOwner();
    void invalidatedResultsAreReportedStale();
    void cancelAllDropsQueueAndKillsRunning();
    void historyOutputGoesToTakenTempFile();

private:
    QString newGate() { return m_repository.filePath(QStringLiteral("gate-%1").arg(++m_gates)); }

    bool m_blockingAvailable = false;
    int m_gates = 0;
    QTemporaryDir m_repository;
};

void GitLaneSchedulerTest::initTestCase()
{
    if (!run(QDir::currentPath(), {QStringLiteral("--version")}).succeeded())
        QSKIP("git is not available");
    QVERIFY(m_repository.isValid());
    QVERIFY(initializeRepository(m_repository.path()));
    const QString gate = newGate();
    open(gate);
    const CommandResult probe = run(m_repository.path(), blocking(gate));
    m_blockingAvailable = probe.succeeded() && probe.stdoutBytes.trimmed() == "waited";
}

void GitLaneSchedulerTest::cleanup()
{
    // Releases whatever a test left waiting, including the sh of a killed git.
    for (int gate = 1; gate <= m_gates; ++gate)
        open(m_repository.filePath(QStringLiteral("gate-%1").arg(gate)));
}

#define REQUIRE_BLOCKING()                                                       \
    do {                                                                         \
        if (!m_blockingAvailable)                                                \
            QSKIP("git shell aliases are unavailable; lanes cannot be held busy"); \
    } while (false)

void GitLaneSchedulerTest::routesOperationsToLanes_data()
{
    QTest::addColumn<QString>("operation");
    QTest::addColumn<int>("lane");
    const auto row = [](const char *operation, Lane lane) {
        QTest::newRow(operation) << QString::fromLatin1(operation) << int(lane);
    };
    row("history", Lane::History);
    row("history-parse", Lane::History);
    row("branches", Lane::History);
    row("worktrees", Lane::History);
    row("discover", Lane::Status);
    row("refresh", Lane::Status);
    row("base-content", Lane::Status);
    row("commit", Lane::Status);
    row("commit-details", Lane::Diff);
    row("commit-files", Lane::Diff);
    row("commit-files-stats", Lane::Diff);
    row("diff-stats", Lane::Diff);
    row("commit-preview", Lane::Peek);
    row("diff-body", Lane::DiffBody);
    row("worktree-status", Lane::Probe);
    row("not-an-operation", Lane::Status);
}

void GitLaneSchedulerTest::routesOperationsToLanes()
{
    QFETCH(QString, operation);
    QFETCH(int, lane);
    QCOMPARE(int(GitLaneScheduler::laneForOperation(operation)), lane);
}

void GitLaneSchedulerTest::rejectsEmptyDirectory()
{
    Recorder recorder;
    GitLaneScheduler scheduler(failureDetail);
    recorder.attach(scheduler);
    Result result;
    QVERIFY(!scheduler.run(QString(), quick(), QStringLiteral("refresh"),
                           recorder.capture(QStringLiteral("refresh"), result), true));
    QVERIFY(!scheduler.busy());
    QVERIFY(!scheduler.anyLaneRunning());
    QVERIFY(recorder.started.isEmpty());
    QTest::qWait(50);
    QVERIFY(!result.called);
}

void GitLaneSchedulerTest::reportsVisibleCompletion()
{
    Recorder recorder;
    GitLaneScheduler scheduler(failureDetail);
    recorder.attach(scheduler);
    Result result;
    QVERIFY(scheduler.run(m_repository.path(), quick(), QStringLiteral("refresh"),
                          recorder.capture(QStringLiteral("refresh"), result), true));
    QVERIFY(scheduler.busy());
    QVERIFY(scheduler.laneBusy(Lane::Status));
    QCOMPARE(scheduler.laneOperation(Lane::Status), QStringLiteral("refresh"));
    QCOMPARE(recorder.started, QStringList{QStringLiteral("refresh")});

    QTRY_VERIFY_WITH_TIMEOUT(result.called, 10000);
    QVERIFY(result.ok);
    QCOMPARE(result.out.trimmed(), QByteArray("true"));
    QVERIFY(!scheduler.busy());
    QVERIFY(!scheduler.laneBusy(Lane::Status));
    QCOMPARE(recorder.finished.size(), 1);
    QCOMPARE(recorder.finished.at(0).operation, QStringLiteral("refresh"));
    QCOMPARE(int(recorder.finished.at(0).outcome), int(OperationOutcome::Succeeded));
    QVERIFY(recorder.finished.at(0).detail.isEmpty());
    QCOMPARE(recorder.busyChanges, 2);
}

void GitLaneSchedulerTest::reportsFailureDetail()
{
    Recorder recorder;
    GitLaneScheduler scheduler(failureDetail);
    recorder.attach(scheduler);
    Result result;
    QVERIFY(scheduler.run(m_repository.path(),
                          {QStringLiteral("rev-parse"), QStringLiteral("--verify"),
                           QStringLiteral("span-no-such-ref")},
                          QStringLiteral("refresh"),
                          recorder.capture(QStringLiteral("refresh"), result), true));
    QTRY_VERIFY_WITH_TIMEOUT(result.called, 10000);
    QVERIFY(!result.ok);
    QVERIFY(!result.err.isEmpty());
    QCOMPARE(recorder.finished.size(), 1);
    QCOMPARE(int(recorder.finished.at(0).outcome), int(OperationOutcome::Failed));
    QCOMPARE(recorder.finished.at(0).detail, failureDetail(result.err));
    QVERIFY(!scheduler.busy());

    // A Git that cannot start fails the request, reports it once, and frees
    // the lane for the next one.
    const QString git = span::core::toolExecutable(span::core::ExternalTool::Git);
    const auto restore = qScopeGuard(
        [git]() { span::core::setToolExecutable(span::core::ExternalTool::Git, git); });
    span::core::setToolExecutable(span::core::ExternalTool::Git,
                                  m_repository.filePath(QStringLiteral("no-such-git.exe")));
    const qsizetype errorsBefore = recorder.errors.size();
    Result unstartable;
    QVERIFY(scheduler.run(m_repository.path(), quick(), QStringLiteral("refresh"),
                          recorder.capture(QStringLiteral("refresh"), unstartable), true));
    QTRY_VERIFY_WITH_TIMEOUT(unstartable.called, 10000);
    QVERIFY(!unstartable.ok);
    QVERIFY(unstartable.err.contains("could not be started"));
    QVERIFY(recorder.errors.size() > errorsBefore);
    QVERIFY(recorder.errors.last().contains(QStringLiteral("could not be started")));
    QCOMPARE(int(recorder.finished.last().outcome), int(OperationOutcome::Failed));
    QTRY_VERIFY_WITH_TIMEOUT(!scheduler.busy(), 3000);
}

void GitLaneSchedulerTest::sameKindSupersedesRunningRequest()
{
    REQUIRE_BLOCKING();
    Recorder recorder;
    GitLaneScheduler scheduler(failureDetail);
    recorder.attach(scheduler);
    Result older;
    Result newer;
    QVERIFY(scheduler.run(m_repository.path(), blocking(newGate()), QStringLiteral("refresh"),
                          recorder.capture(QStringLiteral("older"), older), true));
    QVERIFY(scheduler.laneRunning(Lane::Status));
    // discover and refresh are one kind: the newer request kills the older.
    QVERIFY(scheduler.run(m_repository.path(), quick(), QStringLiteral("discover"),
                          recorder.capture(QStringLiteral("newer"), newer), true));
    QCOMPARE(recorder.superseded, QList<int>{int(Lane::Status)});

    QTRY_VERIFY_WITH_TIMEOUT(newer.called, 4000);
    QVERIFY(newer.ok);
    QTest::qWait(100);
    QVERIFY(!older.called);
    QCOMPARE(recorder.order, QStringList{QStringLiteral("newer")});
    // The superseded request closes its own progress as cancelled before the
    // replacement opens one; each operation is reported under its own name.
    QCOMPARE(recorder.started, (QStringList{QStringLiteral("refresh"), QStringLiteral("discover")}));
    QCOMPARE(recorder.finished.size(), 2);
    QCOMPARE(recorder.finished.at(0).operation, QStringLiteral("refresh"));
    QCOMPARE(int(recorder.finished.at(0).outcome), int(OperationOutcome::Cancelled));
    QCOMPARE(recorder.finished.at(1).operation, QStringLiteral("discover"));
    QCOMPARE(int(recorder.finished.at(1).outcome), int(OperationOutcome::Succeeded));
    QVERIFY(!scheduler.busy());
}

void GitLaneSchedulerTest::sameKindReplacesWaitingRequest()
{
    REQUIRE_BLOCKING();
    Recorder recorder;
    GitLaneScheduler scheduler(failureDetail);
    recorder.attach(scheduler);
    Result running;
    Result first;
    Result second;
    const QString gate = newGate();
    QVERIFY(scheduler.run(m_repository.path(), blocking(gate), QStringLiteral("commit"),
                          recorder.capture(QStringLiteral("commit"), running), false));
    QVERIFY(scheduler.run(m_repository.path(), quick(), QStringLiteral("base-content"),
                          recorder.capture(QStringLiteral("first"), first), false));
    QVERIFY(scheduler.run(m_repository.path(), quick(), QStringLiteral("base-content"),
                          recorder.capture(QStringLiteral("second"), second), false));
    // Replacing a waiting request does not touch the running one.
    QCOMPARE(recorder.superseded, QList<int>{int(Lane::Status)});
    QVERIFY(scheduler.laneRunning(Lane::Status));
    open(gate);

    QTRY_VERIFY_WITH_TIMEOUT(second.called, 10000);
    QTest::qWait(100);
    QVERIFY(running.ok);
    QVERIFY(!first.called);
    QCOMPARE(recorder.order, (QStringList{QStringLiteral("commit"), QStringLiteral("second")}));
    // Silent requests never open progress.
    QVERIFY(recorder.started.isEmpty());
    QVERIFY(recorder.finished.isEmpty());
}

void GitLaneSchedulerTest::otherKindsQueueInArrivalOrder()
{
    REQUIRE_BLOCKING();
    Recorder recorder;
    GitLaneScheduler scheduler(failureDetail);
    recorder.attach(scheduler);
    Result commit;
    Result base;
    Result refresh;
    const QString gate = newGate();
    QVERIFY(scheduler.run(m_repository.path(), blocking(gate), QStringLiteral("commit"),
                          recorder.capture(QStringLiteral("commit"), commit), false));
    QVERIFY(scheduler.run(m_repository.path(), quick(), QStringLiteral("base-content"),
                          recorder.capture(QStringLiteral("base-content"), base), false));
    QVERIFY(!scheduler.refreshQueued());
    QVERIFY(scheduler.run(m_repository.path(), quick(), QStringLiteral("refresh"),
                          recorder.capture(QStringLiteral("refresh"), refresh), false));
    QVERIFY(scheduler.refreshQueued());
    QVERIFY(recorder.superseded.isEmpty());
    open(gate);

    QTRY_VERIFY_WITH_TIMEOUT(refresh.called, 10000);
    // Nothing was killed: every request ran, in arrival order.
    QVERIFY(commit.ok);
    QCOMPARE(commit.out.trimmed(), QByteArray("waited"));
    QVERIFY(base.ok);
    QVERIFY(refresh.ok);
    QCOMPARE(recorder.order, (QStringList{QStringLiteral("commit"), QStringLiteral("base-content"),
                                          QStringLiteral("refresh")}));
    QVERIFY(!scheduler.refreshQueued());
}

void GitLaneSchedulerTest::lanesRunIndependently()
{
    REQUIRE_BLOCKING();
    Recorder recorder;
    GitLaneScheduler scheduler(failureDetail);
    recorder.attach(scheduler);
    Result status;
    bool statusStillRunning = false;
    bool peekCalled = false;
    bool peekOk = false;
    const QString gate = newGate();
    QVERIFY(scheduler.run(m_repository.path(), blocking(gate), QStringLiteral("refresh"),
                          recorder.capture(QStringLiteral("refresh"), status), false));
    QVERIFY(scheduler.run(m_repository.path(), quick(), QStringLiteral("commit-preview"),
                          [&](bool ok, const QByteArray &, const QByteArray &) {
                              peekOk = ok;
                              peekCalled = true;
                              statusStillRunning = scheduler.laneRunning(Lane::Status);
                          },
                          false));
    QTRY_VERIFY_WITH_TIMEOUT(peekCalled, 2000);
    QVERIFY(peekOk);
    // The slow status read did not hold up the preview on its own lane.
    QVERIFY(statusStillRunning);
    QVERIFY(!status.called);
    open(gate);
    QTRY_VERIFY_WITH_TIMEOUT(status.called, 10000);
    QVERIFY(status.ok);
}

void GitLaneSchedulerTest::busyChangedFollowsEveryLane()
{
    Recorder recorder;
    GitLaneScheduler scheduler(failureDetail);
    recorder.attach(scheduler);
    // Per-lane loading flags (history, commit view) are read on busyChanged,
    // so a second lane going busy must notify although `busy()` stays true.
    scheduler.setLaneBusy(Lane::History, true, QStringLiteral("history"));
    QCOMPARE(recorder.busyChanges, 1);
    scheduler.setLaneBusy(Lane::Diff, true, QStringLiteral("commit-details"));
    QCOMPARE(recorder.busyChanges, 2);
    QVERIFY(scheduler.laneBusy(Lane::Diff));
    // A new operation on a busy lane is a change; repeating it is not.
    scheduler.setLaneBusy(Lane::History, true, QStringLiteral("history-parse"));
    QCOMPARE(recorder.busyChanges, 3);
    QCOMPARE(scheduler.laneOperation(Lane::History), QStringLiteral("history-parse"));
    scheduler.setLaneBusy(Lane::History, true, QStringLiteral("history-parse"));
    QCOMPARE(recorder.busyChanges, 3);

    scheduler.setLaneBusy(Lane::History, false);
    QCOMPARE(recorder.busyChanges, 4);
    QVERIFY(scheduler.busy());
    QVERIFY(scheduler.laneBusy(Lane::Diff));
    scheduler.setLaneBusy(Lane::Diff, false);
    QCOMPARE(recorder.busyChanges, 5);
    QVERIFY(!scheduler.busy());
    QCOMPARE(recorder.started,
             (QStringList{QStringLiteral("history"), QStringLiteral("commit-details"),
                          QStringLiteral("history-parse")}));
}

void GitLaneSchedulerTest::queuedVisibleRequestKeepsRunningProgress()
{
    REQUIRE_BLOCKING();
    Recorder recorder;
    GitLaneScheduler scheduler(failureDetail);
    recorder.attach(scheduler);
    Result stats;
    Result details;
    // diff-stats and commit-details share the Diff lane but are different
    // kinds: the second waits, and taking over the lane's busy task must not
    // swallow the running request's own completion progress.
    const QString gate = newGate();
    QVERIFY(scheduler.run(m_repository.path(), blocking(gate), QStringLiteral("diff-stats"),
                          recorder.capture(QStringLiteral("diff-stats"), stats), true));
    QVERIFY(scheduler.run(m_repository.path(), quick(), QStringLiteral("commit-details"),
                          recorder.capture(QStringLiteral("commit-details"), details), true));
    open(gate);
    QTRY_VERIFY_WITH_TIMEOUT(details.called, 10000);
    QCOMPARE(recorder.order, (QStringList{QStringLiteral("diff-stats"),
                                          QStringLiteral("commit-details")}));
    QCOMPARE(recorder.finished.size(), 1);
    QCOMPARE(recorder.finished.at(0).operation, QStringLiteral("diff-stats"));
    QCOMPARE(int(recorder.finished.at(0).outcome), int(OperationOutcome::Succeeded));

    // The commit view owns its busy flag and closes it itself.
    scheduler.finishLaneTask(Lane::Diff, QStringLiteral("commit-details"),
                             OperationOutcome::Succeeded);
    QCOMPARE(recorder.finished.size(), 2);
    QCOMPARE(recorder.finished.at(1).operation, QStringLiteral("commit-details"));
    // Every progress that opened was closed.
    QCOMPARE(recorder.unfinished(), QStringList{});
    QVERIFY(!scheduler.busy());
}

void GitLaneSchedulerTest::progressNamesItsOwnOperation()
{
    REQUIRE_BLOCKING();
    Recorder recorder;
    GitLaneScheduler scheduler(failureDetail);
    recorder.attach(scheduler);
    Result branches;
    Result stats;
    // Another lane is busy with a visible operation of its own.
    const QString gate = newGate();
    QVERIFY(scheduler.run(m_repository.path(), blocking(gate), QStringLiteral("branches"),
                          recorder.capture(QStringLiteral("branches"), branches), true));
    QVERIFY(scheduler.run(m_repository.path(), quick(), QStringLiteral("diff-stats"),
                          recorder.capture(QStringLiteral("diff-stats"), stats), true));
    QTRY_VERIFY_WITH_TIMEOUT(stats.called, 10000);
    QVERIFY(!branches.called);
    QCOMPARE(recorder.finished.size(), 1);
    QCOMPARE(recorder.finished.at(0).operation, QStringLiteral("diff-stats"));
    QVERIFY(scheduler.laneBusy(Lane::History));
    open(gate);

    QTRY_VERIFY_WITH_TIMEOUT(branches.called, 10000);
    QCOMPARE(recorder.finished.size(), 2);
    QCOMPARE(recorder.finished.at(1).operation, QStringLiteral("branches"));
    QVERIFY(!scheduler.busy());
}
void GitLaneSchedulerTest::silentRequestNeverTakesOverVisibleOwner()
{
    REQUIRE_BLOCKING();
    Recorder recorder;
    GitLaneScheduler scheduler(failureDetail);
    recorder.attach(scheduler);
    Result visible;
    Result silent;
    const QString gate = newGate();
    QVERIFY(scheduler.run(m_repository.path(), blocking(gate), QStringLiteral("commit-details"),
                          recorder.capture(QStringLiteral("visible"), visible), true));
    // commit-files is the same commit-view kind, but a background request
    // must not kill what the user asked for.
    QVERIFY(!scheduler.run(m_repository.path(), quick(), QStringLiteral("commit-files"),
                           recorder.capture(QStringLiteral("silent"), silent), false));
    QVERIFY(recorder.superseded.isEmpty());
    open(gate);

    QTRY_VERIFY_WITH_TIMEOUT(visible.called, 10000);
    QVERIFY(visible.ok);
    QVERIFY(!silent.called);
    // The commit view's completion owns the lane's busy flag: the lane stays
    // busy until the owner settles it after its follow-on steps.
    QVERIFY(scheduler.laneBusy(Lane::Diff));
    QVERIFY(scheduler.busy());
    QVERIFY(recorder.finished.isEmpty());
    scheduler.finishLaneTask(Lane::Diff, QStringLiteral("commit-details"),
                             OperationOutcome::Succeeded);
    QVERIFY(!scheduler.laneBusy(Lane::Diff));
    QVERIFY(!scheduler.busy());
    QCOMPARE(recorder.finished.size(), 1);
    QCOMPARE(recorder.finished.at(0).operation, QStringLiteral("commit-details"));
    QCOMPARE(int(recorder.finished.at(0).outcome), int(OperationOutcome::Succeeded));
}

void GitLaneSchedulerTest::invalidatedResultsAreReportedStale()
{
    REQUIRE_BLOCKING();
    Recorder recorder;
    GitLaneScheduler scheduler(failureDetail);
    recorder.attach(scheduler);
    Result result;
    const quint64 generation = scheduler.generation();
    const QString gate = newGate();
    QVERIFY(scheduler.run(m_repository.path(), blocking(gate), QStringLiteral("refresh"),
                          recorder.capture(QStringLiteral("refresh"), result), true));
    scheduler.invalidateResults();
    QCOMPARE(scheduler.generation(), generation + 1);
    open(gate);

    QTRY_COMPARE_WITH_TIMEOUT(recorder.stale, QList<int>{int(Lane::Status)}, 10000);
    // A result from before a repository switch never reaches its completion.
    QVERIFY(!result.called);
    QCOMPARE(recorder.finished.size(), 1);
    QCOMPARE(int(recorder.finished.at(0).outcome), int(OperationOutcome::Cancelled));
    QVERIFY(!scheduler.busy());

    // Requests issued after the switch complete normally.
    Result fresh;
    QVERIFY(scheduler.run(m_repository.path(), quick(), QStringLiteral("refresh"),
                          recorder.capture(QStringLiteral("fresh"), fresh), false));
    QTRY_VERIFY_WITH_TIMEOUT(fresh.called, 10000);
    QVERIFY(fresh.ok);
}

void GitLaneSchedulerTest::cancelAllDropsQueueAndKillsRunning()
{
    REQUIRE_BLOCKING();
    Recorder recorder;
    GitLaneScheduler scheduler(failureDetail);
    recorder.attach(scheduler);
    Result running;
    Result queued;
    QVERIFY(scheduler.run(m_repository.path(), blocking(newGate()), QStringLiteral("refresh"),
                          recorder.capture(QStringLiteral("running"), running), true));
    QVERIFY(scheduler.run(m_repository.path(), quick(), QStringLiteral("base-content"),
                          recorder.capture(QStringLiteral("queued"), queued), false));

    QVERIFY(scheduler.cancelAll());
    QTRY_VERIFY_WITH_TIMEOUT(!scheduler.anyLaneRunning(), 4000);
    QTRY_VERIFY_WITH_TIMEOUT(!recorder.finished.isEmpty(), 4000);
    QTest::qWait(100);
    QVERIFY(!running.called);
    QVERIFY(!queued.called);
    QCOMPARE(recorder.finished.size(), 1);
    QCOMPARE(recorder.finished.at(0).operation, QStringLiteral("refresh"));
    QCOMPARE(int(recorder.finished.at(0).outcome), int(OperationOutcome::Cancelled));
    QVERIFY(!recorder.errors.isEmpty());
    QVERIFY(!recorder.errors.constLast().isEmpty());
    QVERIFY(!scheduler.busy());
    // Nothing left to cancel.
    QVERIFY(!scheduler.cancelAll());
}

void GitLaneSchedulerTest::historyOutputGoesToTakenTempFile()
{
    Recorder recorder;
    GitLaneScheduler scheduler(failureDetail);
    recorder.attach(scheduler);
    QString stdoutFile;
    QByteArray fileContents;
    Result result;
    QVERIFY(scheduler.run(m_repository.path(), quick(), QStringLiteral("history"),
                          [&](bool ok, const QByteArray &out, const QByteArray &err) {
                              result = {true, ok, out, err};
                              stdoutFile = scheduler.takeStdoutFile(Lane::History);
                              fileContents = readFile(stdoutFile);
                          },
                          true));
    QTRY_VERIFY_WITH_TIMEOUT(result.called, 10000);
    QVERIFY(result.ok);
    // History can be huge: it streams to a file instead of memory.
    QVERIFY(result.out.isEmpty());
    QVERIFY(!stdoutFile.isEmpty());
    QCOMPARE(fileContents.trimmed(), QByteArray("true"));
    // Taking hands ownership to the caller exactly once.
    QVERIFY(scheduler.takeStdoutFile(Lane::History).isEmpty());
    QVERIFY(QFile::exists(stdoutFile));
    QFile::remove(stdoutFile);

    // reset() forgets busy state the history completion still owned.
    QVERIFY(scheduler.laneBusy(Lane::History));
    scheduler.reset();
    QVERIFY(!scheduler.laneBusy(Lane::History));
    QVERIFY(!scheduler.busy());
}

QTEST_GUILESS_MAIN(GitLaneSchedulerTest)
#include "git_lane_scheduler_test.moc"
