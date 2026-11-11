#include "git/git_repository_service.h"
#include "git_test_support.h"

#include <QDir>
#include <QFile>
#include <QSignalSpy>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTest>

using namespace span::tests::git;

class GitRepositoryConcurrencyTest final : public QObject {
    Q_OBJECT

private slots:
    void initTestCase();
    void rapidSelectionsKeepLatestResults();
    void projectSwitchDropsStaleResults();
    void rapidCommitSelectionReleasesDiffLaneBusy();
    void sharedLaneQueuesOtherKindsAndSupersedesSameKind();
    void emptyHistoryDoesNotKeepHistoryViewLoading();
};

static bool commitPath(const QString &directory, const QString &path,
                       const QByteArray &contents, const QString &subject)
{
    return writeFile(QDir(directory).filePath(path), contents)
        && run(directory, {QStringLiteral("add"), QStringLiteral("--"), path}).succeeded()
        && run(directory, {QStringLiteral("commit"), QStringLiteral("-m"), subject}).succeeded();
}

// `count` commits, each adding file-<n>.txt with `contents` under subject
// <subjectPrefix><n>.
static QList<ImportedCommit> commitPerFile(const QString &subjectPrefix, const QByteArray &contents, int count)
{
    QList<ImportedCommit> commits;
    for (int index = 0; index < count; ++index)
        commits.append({QStringLiteral("file-%1.txt").arg(index), contents, subjectPrefix + QString::number(index)});
    return commits;
}

static QStringList historySubjects(GitRepositoryService &service)
{
    QStringList subjects;
    for (const QVariant &value : service.history()->items())
        subjects.append(value.toMap().value(QStringLiteral("subject")).toString());
    return subjects;
}

void GitRepositoryConcurrencyTest::initTestCase()
{
    QStandardPaths::setTestModeEnabled(true);
    if (!run(QDir::currentPath(), {QStringLiteral("--version")}).succeeded())
        QSKIP("git is not available");
}

void GitRepositoryConcurrencyTest::rapidSelectionsKeepLatestResults()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    QVERIFY(initializeRepository(dir.path()));
    QVERIFY(commitPath(dir.path(), QStringLiteral("tracked.txt"), "first\n",
                       QStringLiteral("first")));
    const QString firstHash = QString::fromUtf8(
        run(dir.path(), {QStringLiteral("rev-parse"), QStringLiteral("HEAD")}).stdoutBytes).trimmed();
    QVERIFY(commitPath(dir.path(), QStringLiteral("tracked.txt"), "second\n",
                       QStringLiteral("second")));
    const QString secondHash = QString::fromUtf8(
        run(dir.path(), {QStringLiteral("rev-parse"), QStringLiteral("HEAD")}).stdoutBytes).trimmed();
    QVERIFY(run(dir.path(), {QStringLiteral("switch"), QStringLiteral("-c"),
                             QStringLiteral("feature")}).succeeded());
    QVERIFY(commitPath(dir.path(), QStringLiteral("feature.txt"), "feature\n",
                       QStringLiteral("feature-only")));
    QVERIFY(run(dir.path(), {QStringLiteral("switch"), QStringLiteral("main")}).succeeded());

    GitRepositoryService service;
    service.setProjectPath(dir.path());
    GIT_TRY_VERIFY(service.available(), 5000);
    GIT_TRY_VERIFY(!service.busy(), 5000);

    GIT_TRY_VERIFY(service.history()->count() > 0, 10000);
    QCOMPARE(service.currentBranch(), QStringLiteral("main"));
    QVERIFY(!historySubjects(service).contains(QStringLiteral("feature-only")));

    service.selectHistoryCommit(firstHash);
    service.selectHistoryCommit(secondHash);
    GIT_TRY_VERIFY(!service.busy(), 10000);
    QCOMPARE(service.selectedCommit(), secondHash);
    QCOMPARE(service.selectedCommitDetails().value(QStringLiteral("subject")).toString(),
             QStringLiteral("second"));
}

void GitRepositoryConcurrencyTest::projectSwitchDropsStaleResults()
{
    QTemporaryDir first;
    QTemporaryDir second;
    QVERIFY(first.isValid());
    QVERIFY(second.isValid());
    QVERIFY(initializeRepository(first.path()));
    QVERIFY(initializeRepository(second.path()));
    QVERIFY(importCommits(first.path(), commitPerFile(QStringLiteral("old-"), "old\n", 40)));
    QVERIFY(commitPath(second.path(), QStringLiteral("current.txt"), "new\n",
                       QStringLiteral("second-only")));

    GitRepositoryService service;
    service.setProjectPath(first.path());
    GIT_TRY_VERIFY(service.available(), 8000);
    service.setProjectPath(second.path());
    GIT_TRY_VERIFY(service.available(), 8000);
    GIT_TRY_COMPARE(QDir::cleanPath(service.repositoryRoot()),
                              QDir::cleanPath(second.path()), 8000);
    GIT_TRY_VERIFY(!service.busy(), 10000);
    GIT_TRY_VERIFY(service.history()->count() > 0, 10000);
    QCOMPARE(service.currentBranch(), QStringLiteral("main"));
    QCOMPARE(service.history()->itemAt(0).value(QStringLiteral("subject")).toString(),
             QStringLiteral("second-only"));
    QCOMPARE(historySubjects(service).filter(QStringLiteral("old-")), QStringList{});
}

void GitRepositoryConcurrencyTest::rapidCommitSelectionReleasesDiffLaneBusy()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    QVERIFY(initializeRepository(dir.path()));
    QVERIFY(commitPath(dir.path(), QStringLiteral("a.txt"), "one\n", QStringLiteral("c1")));
    const QString firstHash = QString::fromUtf8(
        run(dir.path(), {QStringLiteral("rev-parse"), QStringLiteral("HEAD")}).stdoutBytes).trimmed();
    QVERIFY(commitPath(dir.path(), QStringLiteral("a.txt"), "two\n", QStringLiteral("c2")));
    const QString secondHash = QString::fromUtf8(
        run(dir.path(), {QStringLiteral("rev-parse"), QStringLiteral("HEAD")}).stdoutBytes).trimmed();
    QVERIFY(commitPath(dir.path(), QStringLiteral("a.txt"), "three\n", QStringLiteral("c3")));
    const QString thirdHash = QString::fromUtf8(
        run(dir.path(), {QStringLiteral("rev-parse"), QStringLiteral("HEAD")}).stdoutBytes).trimmed();

    GitRepositoryService service;
    service.setProjectPath(dir.path());
    GIT_TRY_VERIFY(service.available(), 5000);
    GIT_TRY_VERIFY(!service.busy(), 5000);

    service.selectHistoryCommit(firstHash);
    service.selectHistoryCommit(secondHash);
    service.selectHistoryCommit(thirdHash);

    GIT_TRY_VERIFY(!service.commitViewLoading(), 5000);
    GIT_TRY_VERIFY(!service.busy(), 5000);
    QCOMPARE(service.selectedCommit(), thirdHash);
}

void GitRepositoryConcurrencyTest::sharedLaneQueuesOtherKindsAndSupersedesSameKind()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    QVERIFY(initializeRepository(dir.path()));
    QVERIFY(importCommits(dir.path(), commitPerFile(QStringLiteral("commit-"), "line\n", 40)));

    GitRepositoryService service;
    service.setProjectPath(dir.path());
    GIT_TRY_VERIFY(service.available(), 8000);
    GIT_TRY_VERIFY(!service.busy(), 8000);
    const QDir root(service.repositoryRoot());
    QVERIFY(writeFile(root.filePath(QStringLiteral("file-1.txt")), "changed\n"));

    // Refresh and base-content share the Status lane but are different kinds:
    // the reads queue behind the running refresh instead of killing it. Two
    // base-content reads are one kind: only the newer one runs.
    QSignalSpy baseSpy(&service, &GitRepositoryService::baseContentReady);
    service.refresh();
    service.requestBaseContent(root.filePath(QStringLiteral("file-0.txt")));
    service.requestBaseContent(root.filePath(QStringLiteral("file-2.txt")));
    GIT_TRY_COMPARE(service.statusMap().value(QStringLiteral("file-1.txt")).toString(),
                              QStringLiteral("modified"), 8000);
    GIT_TRY_VERIFY(!service.busy(), 8000);
    GIT_TRY_COMPARE(baseSpy.count(), 1, 8000);
    QTest::qWait(200);
    QCOMPARE(baseSpy.count(), 1);
    QCOMPARE(baseSpy.at(0).at(0).toString(), root.filePath(QStringLiteral("file-2.txt")));
    QCOMPARE(baseSpy.at(0).at(1).toString(), QStringLiteral("line\n"));
}

void GitRepositoryConcurrencyTest::emptyHistoryDoesNotKeepHistoryViewLoading()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    QVERIFY(initializeRepository(dir.path()));

    GitRepositoryService service;
    service.setProjectPath(dir.path());
    GIT_TRY_VERIFY(service.available(), 5000);
    GIT_TRY_VERIFY(!service.busy(), 5000);

    QVERIFY(!service.historyViewLoading());
}

QTEST_MAIN(GitRepositoryConcurrencyTest)
#include "git_repository_concurrency_test.moc"
