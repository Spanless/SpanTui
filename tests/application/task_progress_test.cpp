#include "application/task_progress.h"
#include "application/task_progress_binding.h"

#include <QCoreApplication>
#include <QTest>

using span::application::TaskProgressService;

namespace {

// The product reveals after 160 ms and lingers 1.6 s; tests keep the order of
// events, not those lengths.
constexpr int kRevealMs = 30;
constexpr int kLingerMs = 150;

QVariant primaryData(const TaskProgressService &tasks, TaskProgressService::Role role)
{
    return tasks.data(tasks.index(0), role);
}

QStringList gitSources(const QStringList &operations)
{
    QStringList sources;
    for (const QString &operation : operations)
        sources.append(span::application::taskProgressSourceForGitOperation(operation));
    return sources;
}

} // namespace

class TaskProgressTest final : public QObject {
    Q_OBJECT

private slots:
    void invalidAndUnknownSourcesAreIgnored();
    void delayedRevealAndFastSuccessStayQuiet();
    void failedAndCancelledTasksLinger();
    void restartClearsPreviousState();
    void determinateProgressIsMonotonic();
    void newestRunningTaskIsPrimary();
    void ownerSourceMapping_data();
    void ownerSourceMapping();
    void gitOperationsOfOneViewShareASource();
    void bindingMapsSourceDetailAndOutcome();
    void sourcedBindingKeepsConcurrentSources();
    void perOwnerBindingsDoNotOverwriteEachOther();
    void perOwnerSourcedEntriesStayIsolated();
    void destroyingBindingCancelsActiveSources();
};

void TaskProgressTest::invalidAndUnknownSourcesAreIgnored()
{
    TaskProgressService tasks;
    tasks.setTimingForTesting(kRevealMs, kLingerMs);
    QVERIFY(!tasks.start({}, QStringLiteral("Pushing")));
    QVERIFY(!tasks.start(QStringLiteral("git"), {}));
    tasks.update(QStringLiteral("missing"), 0.4, QStringLiteral("ignored"));
    tasks.finish(QStringLiteral("missing"));
    QCOMPARE(tasks.count(), 0);
}

void TaskProgressTest::delayedRevealAndFastSuccessStayQuiet()
{
    TaskProgressService tasks;
    tasks.setTimingForTesting(kRevealMs, kLingerMs);
    QVERIFY(tasks.start(QStringLiteral("git"), QStringLiteral("Pushing")));
    QCOMPARE(tasks.count(), 0);
    QCOMPARE(tasks.activeCount(), 0);
    tasks.finish(QStringLiteral("git"), TaskProgressService::Succeeded);
    QTest::qWait(kRevealMs * 3);
    QCOMPARE(tasks.count(), 0);

    QVERIFY(tasks.start(QStringLiteral("git"), QStringLiteral("Fetching")));
    QTRY_COMPARE_WITH_TIMEOUT(tasks.count(), 1, 500);
    QCOMPARE(tasks.activeCount(), 1);
    QCOMPARE(tasks.primaryTitle(), QStringLiteral("Fetching"));
}

void TaskProgressTest::failedAndCancelledTasksLinger()
{
    TaskProgressService tasks;
    tasks.setTimingForTesting(kRevealMs, kLingerMs);
    QVERIFY(tasks.start(QStringLiteral("git"), QStringLiteral("Fetching")));
    tasks.finish(QStringLiteral("git"), TaskProgressService::Failed,
                 QStringLiteral("Network unavailable"));
    QCOMPARE(tasks.count(), 1);
    QCOMPARE(tasks.activeCount(), 0);
    QCOMPARE(tasks.primaryOutcome(), int(TaskProgressService::Failed));
    QCOMPARE(primaryData(tasks, TaskProgressService::DetailRole).toString(), QStringLiteral("Network unavailable"));
    QTRY_COMPARE_WITH_TIMEOUT(tasks.count(), 0, 1000);

    QVERIFY(tasks.start(QStringLiteral("update"), QStringLiteral("Checking")));
    tasks.finish(QStringLiteral("update"), TaskProgressService::Cancelled,
                 QStringLiteral("Cancelled by user"));
    QCOMPARE(tasks.count(), 1);
    QCOMPARE(tasks.primaryOutcome(), int(TaskProgressService::Cancelled));
    tasks.dismiss(QStringLiteral("update"));
    QCOMPARE(tasks.count(), 0);
}

void TaskProgressTest::restartClearsPreviousState()
{
    TaskProgressService tasks;
    tasks.setTimingForTesting(kRevealMs, kLingerMs);
    QVERIFY(tasks.start(QStringLiteral("git"), QStringLiteral("Fetching")));
    tasks.finish(QStringLiteral("git"), TaskProgressService::Failed,
                 QStringLiteral("Old failure"));
    QCOMPARE(tasks.count(), 1);
    QVERIFY(tasks.start(QStringLiteral("git"), QStringLiteral("Pushing")));
    QCOMPARE(tasks.primaryTitle(), QStringLiteral("Pushing"));
    QCOMPARE(primaryData(tasks, TaskProgressService::DetailRole).toString(), QString());
    QCOMPARE(primaryData(tasks, TaskProgressService::ProgressRole).toReal(), -1.0);
    QCOMPARE(tasks.primaryOutcome(), int(TaskProgressService::Running));
}

void TaskProgressTest::determinateProgressIsMonotonic()
{
    TaskProgressService tasks;
    tasks.setTimingForTesting(kRevealMs, kLingerMs);
    QVERIFY(tasks.start(QStringLiteral("git"), QStringLiteral("Pushing")));
    tasks.update(QStringLiteral("git"), 0.6, QStringLiteral("Writing objects"));
    tasks.update(QStringLiteral("git"), 0.2, QStringLiteral("Finalizing"));
    tasks.update(QStringLiteral("git"), -1, QStringLiteral("Waiting"));
    QTRY_COMPARE_WITH_TIMEOUT(tasks.count(), 1, 500);
    QCOMPARE(primaryData(tasks, TaskProgressService::ProgressRole).toReal(), 0.6);
    QVERIFY(primaryData(tasks, TaskProgressService::DeterminateRole).toBool());
    QCOMPARE(primaryData(tasks, TaskProgressService::DetailRole).toString(), QStringLiteral("Waiting"));
}

void TaskProgressTest::newestRunningTaskIsPrimary()
{
    TaskProgressService tasks;
    tasks.setTimingForTesting(kRevealMs, kLingerMs);
    QVERIFY(tasks.start(QStringLiteral("files"), QStringLiteral("Loading files")));
    QTest::qWait(10);
    QVERIFY(tasks.start(QStringLiteral("git"), QStringLiteral("Fetching")));
    QTRY_COMPARE_WITH_TIMEOUT(tasks.count(), 2, 500);
    QCOMPARE(tasks.primaryTitle(), QStringLiteral("Fetching"));
    QCOMPARE(tasks.activeCount(), 2);

    tasks.finish(QStringLiteral("git"), TaskProgressService::Failed,
                 QStringLiteral("Failed"));
    QCOMPARE(tasks.primaryTitle(), QStringLiteral("Loading files"));
    QCOMPARE(tasks.primaryOutcome(), int(TaskProgressService::Running));
    QCOMPARE(tasks.activeCount(), 1);

    // Restarting a lower row brings it back to the top.
    QVERIFY(tasks.start(QStringLiteral("git"), QStringLiteral("Fetching again")));
    QTRY_COMPARE_WITH_TIMEOUT(tasks.primaryTitle(), QStringLiteral("Fetching again"), 500);
}

void TaskProgressTest::ownerSourceMapping_data()
{
    QTest::addColumn<span::application::ProgressOwner>("owner");
    QTest::addColumn<QString>("source");
    QTest::newRow("git") << span::application::ProgressOwner::Git
                         << QStringLiteral("git");
    QTest::newRow("git-history") << span::application::ProgressOwner::GitHistory
                                << QStringLiteral("git-history");
    QTest::newRow("git-diff") << span::application::ProgressOwner::GitDiff
                             << QStringLiteral("git-diff");
    QTest::newRow("update") << span::application::ProgressOwner::Update
                            << QStringLiteral("update");
    QTest::newRow("files") << span::application::ProgressOwner::Files
                           << QStringLiteral("files");
    QTest::newRow("document") << span::application::ProgressOwner::Document
                              << QStringLiteral("document");
}

void TaskProgressTest::ownerSourceMapping()
{
    QFETCH(span::application::ProgressOwner, owner);
    QFETCH(QString, source);
    QCOMPARE(span::application::taskProgressSource(owner), source);

    TaskProgressService tasks;
    tasks.setTimingForTesting(kRevealMs, kLingerMs);
    span::application::TaskProgressBinding binding(&tasks, owner);
    QCOMPARE(binding.source(), source);
    binding.progressStarted(QStringLiteral("Owner title"));
    binding.progressChanged(0.25, QStringLiteral("Owner detail"));
    QTRY_COMPARE_WITH_TIMEOUT(tasks.count(), 1, 500);
    QCOMPARE(tasks.data(tasks.index(0), TaskProgressService::SourceRole).toString(),
             source);
    QCOMPARE(tasks.primaryTitle(), QStringLiteral("Owner title"));
    QCOMPARE(primaryData(tasks, TaskProgressService::DetailRole).toString(), QStringLiteral("Owner detail"));
    binding.progressFinished(span::core::OperationOutcome::Succeeded,
                             QString());
    QCOMPARE(tasks.count(), 0);
}

void TaskProgressTest::gitOperationsOfOneViewShareASource()
{
    // A superseding step closes the progress the previous one opened, so all
    // operations of one view report under one source; views that run side
    // by side never share one.
    const QString history = gitSources({QStringLiteral("history")}).constFirst();
    QCOMPARE(gitSources({QStringLiteral("history-parse"), QStringLiteral("branches"), QStringLiteral("refresh"),
                         QStringLiteral("discover")}),
             QStringList(4, history));
    const QString commitView = gitSources({QStringLiteral("commit-details")}).constFirst();
    QCOMPARE(gitSources({QStringLiteral("commit-files"), QStringLiteral("commit-files-stats")}),
             QStringList(2, commitView));
    const QString general = gitSources({QStringLiteral("commit")}).constFirst();
    QCOMPARE(
        gitSources({QStringLiteral("diff-stats"), QStringLiteral("diff-body"), QStringLiteral("not-an-operation")}),
        QStringList(3, general));
    QVERIFY(history != commitView);
    QVERIFY(history != general);
    QVERIFY(commitView != general);
}

void TaskProgressTest::sourcedBindingKeepsConcurrentSources()
{
    TaskProgressService tasks;
    tasks.setTimingForTesting(kRevealMs, kLingerMs);
    span::application::TaskProgressBinding binding(&tasks, QString());
    binding.progressStartedOn(QStringLiteral("git-history"),
                              QStringLiteral("Loading history"));
    binding.progressStartedOn(QStringLiteral("git-diff"),
                              QStringLiteral("Preparing diff"));
    QTRY_COMPARE_WITH_TIMEOUT(tasks.count(), 2, 500);
    QCOMPARE(tasks.activeCount(), 2);
    binding.progressFinishedOn(QStringLiteral("git-diff"),
                               span::core::OperationOutcome::Succeeded,
                               QString());
    QCOMPARE(tasks.count(), 1);
    QCOMPARE(tasks.primaryTitle(), QStringLiteral("Loading history"));
}

void TaskProgressTest::bindingMapsSourceDetailAndOutcome()
{
    TaskProgressService tasks;
    tasks.setTimingForTesting(kRevealMs, kLingerMs);
    span::application::TaskProgressBinding binding(
        &tasks, QStringLiteral("document"));
    QCOMPARE(binding.source(), QStringLiteral("document"));
    binding.progressStarted(QStringLiteral("Saving guide.md"));
    binding.progressChanged(0.5, QStringLiteral("Writing data"));
    QTRY_COMPARE_WITH_TIMEOUT(tasks.count(), 1, 500);
    QCOMPARE(tasks.data(tasks.index(0), TaskProgressService::SourceRole).toString(),
             QStringLiteral("document"));
    QCOMPARE(tasks.primaryTitle(), QStringLiteral("Saving guide.md"));
    QCOMPARE(primaryData(tasks, TaskProgressService::DetailRole).toString(), QStringLiteral("Writing data"));
    binding.progressFinished(span::core::OperationOutcome::Failed,
                             QStringLiteral("Disk full"));
    QCOMPARE(tasks.primaryOutcome(), int(TaskProgressService::Failed));
    QCOMPARE(primaryData(tasks, TaskProgressService::DetailRole).toString(), QStringLiteral("Disk full"));
}

void TaskProgressTest::perOwnerBindingsDoNotOverwriteEachOther()
{
    TaskProgressService tasks;
    tasks.setTimingForTesting(kRevealMs, kLingerMs);
    QObject firstOwner;
    QObject secondOwner;
    {
        span::application::TaskProgressBinding first(
            &tasks, span::application::ProgressOwner::Files, &firstOwner);
        span::application::TaskProgressBinding second(
            &tasks, span::application::ProgressOwner::Files, &secondOwner);
        QVERIFY(first.source() != second.source());
        first.progressStarted(QStringLiteral("Indexing first"));
        second.progressStarted(QStringLiteral("Indexing second"));
        QTRY_COMPARE(tasks.count(), 2);
        first.progressFinished(span::core::OperationOutcome::Succeeded, {});
        QCOMPARE(tasks.count(), 1);
        QCOMPARE(tasks.data(tasks.index(0), TaskProgressService::TitleRole).toString(),
                 QStringLiteral("Indexing second"));
    }
}

void TaskProgressTest::perOwnerSourcedEntriesStayIsolated()
{
    TaskProgressService tasks;
    tasks.setTimingForTesting(kRevealMs, kLingerMs);
    QObject firstOwner;
    QObject secondOwner;
    // The second binding outlives the inner scope so only the first binding's
    // destructor runs mid-test.
    span::application::TaskProgressBinding second(&tasks, QString(), &secondOwner);
    {
        span::application::TaskProgressBinding first(&tasks, QString(), &firstOwner);
        first.progressStartedOn(QStringLiteral("git-history"),
                                QStringLiteral("Loading first history"));
        second.progressStartedOn(QStringLiteral("git-history"),
                                 QStringLiteral("Loading second history"));
        QTRY_COMPARE(tasks.count(), 2);
        const QString firstKey = tasks.data(tasks.index(0),
                                            TaskProgressService::SourceRole)
                                     .toString();
        const QString secondKey = tasks.data(tasks.index(1),
                                             TaskProgressService::SourceRole)
                                      .toString();
        QVERIFY(firstKey.startsWith(QStringLiteral("git-history#")));
        QVERIFY(secondKey.startsWith(QStringLiteral("git-history#")));
        QVERIFY(firstKey != secondKey);
    }
    // Destroying the first binding must cancel only its own scoped key; the
    // sibling owner's same-named operation keeps running.
    QCOMPARE(tasks.activeCount(), 1);
    QCOMPARE(tasks.primaryTitle(), QStringLiteral("Loading second history"));
    QCOMPARE(tasks.primaryOutcome(), int(TaskProgressService::Running));
}

void TaskProgressTest::destroyingBindingCancelsActiveSources()
{
    TaskProgressService tasks;
    tasks.setTimingForTesting(kRevealMs, kLingerMs);
    QObject owner;
    {
        span::application::TaskProgressBinding binding(
            &tasks, span::application::ProgressOwner::Document, &owner);
        binding.progressStarted(QStringLiteral("Writing guide.md"));
        QTRY_COMPARE(tasks.activeCount(), 1);
    }
    // The owner disappeared mid-operation: the entry must not stay running.
    QCOMPARE(tasks.activeCount(), 0);
    QCOMPARE(tasks.primaryOutcome(), int(TaskProgressService::Cancelled));
}

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    TaskProgressTest test;
    return QTest::qExec(&test, argc, argv);
}

#include "task_progress_test.moc"
