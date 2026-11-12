#include "git/git_constants.h"
#include "git/git_repository_service.h"
#include "git_test_support.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>
#include <QVariantMap>

using span::tests::git::importCommits;
using span::tests::git::ImportedCommit;
using span::tests::git::initializeRepository;
using span::tests::git::run;
using span::tests::git::writeFile;

class GitRepositoryReadTest final : public QObject {
    Q_OBJECT

private slots:
    void initTestCase();
    void readsRepositoryStatesAndLinkedWorktree();
    void refreshesRefsAndWorktreesOnRequest();
    void probesWorktreeChangesInBackground();
    void pollIntervalFollowsIdleFlag();
    void pollRunsStatusOnlyAfterWorkTreeChanges();
    void baseContentKeepsRefreshChain();
    void loadsHistoryOfBranchNamedLikeDirectory();
    void selectsCommitInSha256Repository();
    void servesDiffBodiesBeforeFirstCommit();
    void pagesHistoryAndPrependsNewCommits();
    void backgroundProjectPausesUntilShown();
    void ordinaryDirectoryWaitsForRepository();
    void dropsDiffBodyOfPreviousSelection();

private:
    QString gitTracePath() const;
    int gitStatusRuns() const;

    QTemporaryDir m_traceDirectory;
};

namespace {

QString subjectAt(GitRepositoryService &service, int row)
{
    return service.history()->itemAt(row).value(QStringLiteral("subject")).toString();
}

QString revParse(const QString &repository, const QString &revision)
{
    return QString::fromUtf8(run(repository, {QStringLiteral("rev-parse"), revision}).stdoutBytes).trimmed();
}

bool commitFile(const QString &repository, const QString &path, const QByteArray &contents, const QString &subject)
{
    return writeFile(QDir(repository).filePath(path), contents)
        && run(repository, {QStringLiteral("add"), QStringLiteral("--"), path}).succeeded()
        && run(repository, {QStringLiteral("commit"), QStringLiteral("-m"), subject}).succeeded();
}

// The row of `model` whose "path" is `path`, or an empty map.
QVariantMap rowWithPath(GitItemModel *model, const QString &path)
{
    for (int row = 0; row < model->count(); ++row) {
        const QVariantMap item = model->itemAt(row);
        if (item.value(QStringLiteral("path")).toString() == path) return item;
    }
    return QVariantMap();
}

// The row of `model` whose "path" names the same directory as `path`
// (git may spell the drive letter in another case), or an empty map.
QVariantMap worktreeAt(GitItemModel *model, const QString &path)
{
    for (int row = 0; row < model->count(); ++row) {
        const QVariantMap item = model->itemAt(row);
        if (item.value(QStringLiteral("path")).toString().compare(QDir::cleanPath(path), Qt::CaseInsensitive) == 0)
            return item;
    }
    return QVariantMap();
}

// The ref row of `kind` named `name`, or an empty map.
QVariantMap refRow(GitItemModel *branches, const QString &kind, const QString &name)
{
    for (int row = 0; row < branches->count(); ++row) {
        const QVariantMap item = branches->itemAt(row);
        if (item.value(QStringLiteral("kind")).toString() == kind
            && item.value(QStringLiteral("name")).toString() == name)
            return item;
    }
    return QVariantMap();
}

bool hasRef(GitItemModel *branches, const QString &kind, const QString &name)
{
    return !refRow(branches, kind, name).isEmpty();
}

bool headDecorated(GitItemModel *history, int row)
{
    for (const QVariant &value : history->itemAt(row).value(QStringLiteral("decorations")).toList()) {
        if (value.toMap().value(QStringLiteral("head")).toBool()) return true;
    }
    return false;
}

// New-side text of the added lines in a diff body.
QStringList addedTexts(const QVariant &body)
{
    QStringList added;
    for (const QVariant &line : body.toMap().value(QStringLiteral("lines")).toList()) {
        const QVariantMap row = line.toMap();
        if (row.value(QStringLiteral("kind")).toString() == QLatin1String("add"))
            added.append(row.value(QStringLiteral("newText")).toString());
    }
    return added;
}

// Adds `count` detached worktrees without checkouts, extra-0 upwards in `root`.
bool addDetachedWorktrees(const QString &repository, const QTemporaryDir &root, int count)
{
    for (int index = 0; index < count; ++index) {
        if (!run(repository,
                 {QStringLiteral("worktree"), QStringLiteral("add"), QStringLiteral("--detach"),
                  QStringLiteral("--no-checkout"), root.filePath(QStringLiteral("extra-%1").arg(index))})
                 .succeeded())
            return false;
    }
    return true;
}

// `count` commits of file.txt holding their index, subjects commit-0 upwards.
QList<ImportedCommit> numberedCommits(int count)
{
    QList<ImportedCommit> commits;
    for (int index = 0; index < count; ++index)
        commits.append(
            {QStringLiteral("file.txt"), QByteArray::number(index) + '\n', QStringLiteral("commit-%1").arg(index)});
    return commits;
}

// Asks for tracked.txt's HEAD text once, the first time the service is
// loading changes of a discovered repository.
void requestBaseContentWhileLoading(GitRepositoryService &service, bool *requested)
{
    QObject::connect(&service, &GitRepositoryService::busyChanged, &service, [&service, requested]() {
        // Discovery also counts as loading; wait for the root it finds.
        if (!*requested && service.changesViewLoading() && !service.repositoryRoot().isEmpty()) {
            *requested = true;
            service.requestBaseContent(QDir(service.repositoryRoot()).filePath(QStringLiteral("tracked.txt")));
        }
    });
}

} // namespace

void GitRepositoryReadTest::initTestCase()
{
    if (!run(QDir::currentPath(), {QStringLiteral("--version")}).succeeded()) QSKIP("git is not available");
    // Test directories live under this checkout's .dev\tmp; without a
    // ceiling, git would find the checkout itself from a plain directory.
    qputenv("GIT_CEILING_DIRECTORIES", QDir::toNativeSeparators(QDir::tempPath()).toLocal8Bit());
    // Every git process the services start appends to this trace (their
    // environment is captured once, so it is set before any of them runs).
    QVERIFY(m_traceDirectory.isValid());
    qputenv("GIT_TRACE", QDir::toNativeSeparators(gitTracePath()).toLocal8Bit());
}

QString GitRepositoryReadTest::gitTracePath() const
{
    return m_traceDirectory.filePath(QStringLiteral("git-trace.log"));
}

int GitRepositoryReadTest::gitStatusRuns() const
{
    QFile trace(gitTracePath());
    if (!trace.open(QIODevice::ReadOnly)) return 0;
    return int(trace.readAll().count("built-in: git status"));
}

void GitRepositoryReadTest::readsRepositoryStatesAndLinkedWorktree()
{
    QTemporaryDir root;
    QVERIFY(root.isValid());
    const QString repository = root.filePath(QStringLiteral("repository"));
    const QString nested = QDir(repository).filePath(QStringLiteral("nested/project"));
    const QString worktree = root.filePath(QStringLiteral("linked"));
    QVERIFY(QDir().mkpath(nested));
    QVERIFY(initializeRepository(repository));

    QVERIFY(writeFile(QDir(repository).filePath(QStringLiteral("tracked.txt")), "first\n"));
    QVERIFY(run(repository, {QStringLiteral("add"), QStringLiteral("tracked.txt")}).succeeded());
    QVERIFY(run(repository,
                {QStringLiteral("commit"), QStringLiteral("-m"), QStringLiteral("initial")})
                .succeeded());
    QVERIFY(writeFile(QDir(repository).filePath(QStringLiteral("second.txt")), "second\n"));
    QVERIFY(run(repository, {QStringLiteral("add"), QStringLiteral("second.txt")}).succeeded());
    QVERIFY(run(repository,
                {QStringLiteral("commit"), QStringLiteral("-m"), QStringLiteral("second")})
                .succeeded());
    QVERIFY(writeFile(QDir(repository).filePath(QStringLiteral("tracked.txt")), "changed\n"));
    QVERIFY(writeFile(QDir(repository).filePath(QStringLiteral("untracked.txt")), "new\n"));
    QVERIFY(QDir(repository).mkpath(QStringLiteral("fresh")));
    QVERIFY(writeFile(QDir(repository).filePath(QStringLiteral("fresh/inner.txt")), "a\nb\n"));

    {
        GitRepositoryService service;
        service.setProjectPath(nested);
        GIT_TRY_VERIFY(service.available(), 5000);
        GIT_TRY_VERIFY(!service.busy(), 5000);
        QCOMPARE(QDir::cleanPath(service.repositoryRoot()), QDir::cleanPath(repository));
        QCOMPARE(service.currentBranch(), QStringLiteral("main"));
        GIT_TRY_VERIFY(service.history()->count() >= 2, 5000);
        QCOMPARE(service.history()->itemAt(0).value(QStringLiteral("subject")).toString(),
                 QStringLiteral("second"));
        const QVariantMap statuses = service.statusMap();
        QCOMPARE(statuses.value(QStringLiteral("tracked.txt")).toString(),
                 QStringLiteral("modified"));
        QCOMPARE(statuses.value(QStringLiteral("untracked.txt")).toString(),
                 QStringLiteral("added"));
        // A brand-new directory is listed file by file, not as one "fresh/" row.
        QCOMPARE(statuses.value(QStringLiteral("fresh/inner.txt")).toString(),
                 QStringLiteral("added"));
        QVERIFY(!statuses.contains(QStringLiteral("fresh/")));

        GIT_TRY_VERIFY(service.commitChanges()->count() == 3, 5000);
        // Rows never carry diff bodies; bodies are fetched per file on demand.
        const auto bodyLines = [&service](const QString &scope, const QString &path) {
            return service.diffLines(scope, path).toMap().value(QStringLiteral("lines")).toList();
        };
        const auto changeRow = [&service](const QString &path) { return rowWithPath(service.commitChanges(), path); };
        QVERIFY(!changeRow(QStringLiteral("tracked.txt")).contains(QStringLiteral("lines")));
        // `git diff HEAD --numstat` skips untracked files; their rows and the
        // totals still count their lines without opening a body.
        GIT_TRY_COMPARE(
            changeRow(QStringLiteral("untracked.txt")).value(QStringLiteral("additions")).toInt(), 1,
            5000);
        GIT_TRY_COMPARE(
            changeRow(QStringLiteral("fresh/inner.txt")).value(QStringLiteral("additions")).toInt(), 2,
            5000);
        GIT_TRY_COMPARE(service.diffAdditions(), 4, 5000);
        QCOMPARE(service.diffDeletions(), 1);
        QSignalSpy linesSpy(&service, &GitRepositoryService::diffLinesChanged);
        service.requestDiffLines(QStringLiteral("changes"),
                                 {QStringLiteral("tracked.txt"), QStringLiteral("untracked.txt")});
        GIT_TRY_COMPARE(bodyLines(QStringLiteral("changes"), QStringLiteral("tracked.txt"))
                                      .size(), 2, 5000);
        // `git diff HEAD` skips untracked files: the body comes from disk and
        // the row picks up its line count.
        GIT_TRY_COMPARE(
            bodyLines(QStringLiteral("changes"), QStringLiteral("untracked.txt")).size(), 1, 5000);
        QCOMPARE(changeRow(QStringLiteral("untracked.txt")).value(QStringLiteral("additions")).toInt(),
                 1);
        QVERIFY(linesSpy.count() >= 1);
        QCOMPARE(linesSpy.constLast().at(0).toString(), QStringLiteral("changes"));

        const QString firstHash = service.history()->itemAt(0).value(QStringLiteral("hash")).toString();
        QVERIFY(!firstHash.isEmpty());

        QVERIFY(service.commitPreview(firstHash).isEmpty());
        QSignalSpy previewSpy(&service, &GitRepositoryService::commitPreviewReady);
        service.requestCommitPreview(firstHash);
        GIT_TRY_COMPARE(previewSpy.count(), 1, 5000);
        QCOMPARE(previewSpy.at(0).at(0).toString(), firstHash);
        const QVariantMap preview = service.commitPreview(firstHash);
        QCOMPARE(preview.value(QStringLiteral("hash")).toString(), firstHash);
        QCOMPARE(preview.value(QStringLiteral("filesChanged")).toInt(), 1);
        QCOMPARE(preview.value(QStringLiteral("insertions")).toInt(), 1);
        QVERIFY(service.errorMessage().isEmpty());
        service.selectHistoryCommit(firstHash);
        service.refresh();
        GIT_TRY_VERIFY(!service.commitViewLoading(), 5000);
        GIT_TRY_VERIFY(service.commitDiffFiles()->count() >= 1, 5000);
        QVERIFY(!service.commitDiffFiles()->itemAt(0).contains(QStringLiteral("lines")));
        QVERIFY(service.diffLines(QStringLiteral("commit"), QStringLiteral("second.txt")).isNull());
        // Only rows of the list are served: tracked.txt is not in this commit.
        service.requestDiffLines(QStringLiteral("commit"),
                                 {QStringLiteral("tracked.txt"), QStringLiteral("second.txt")});
        GIT_TRY_COMPARE(
            bodyLines(QStringLiteral("commit"), QStringLiteral("second.txt")).size(), 1, 5000);
        QVERIFY(service.diffLines(QStringLiteral("commit"), QStringLiteral("tracked.txt")).isNull());
        QVERIFY(service.errorMessage().isEmpty());
        const int commitFileCount = service.commitDiffFiles()->count();
        service.refresh();
        GIT_TRY_VERIFY(!service.changesViewLoading(), 5000);
        QCOMPARE(service.commitDiffFiles()->count(), commitFileCount);
        // Selecting another commit drops the previous commit's bodies.
        service.selectHistoryCommit(service.history()->itemAt(1).value(QStringLiteral("hash")).toString());
        QVERIFY(service.diffLines(QStringLiteral("commit"), QStringLiteral("second.txt")).isNull());

        QVERIFY(run(repository, {QStringLiteral("checkout"), QStringLiteral("--detach")})
                    .succeeded());
        service.refresh();
        GIT_TRY_COMPARE(service.currentBranch(), QStringLiteral("(detached)"), 5000);
        GIT_TRY_VERIFY(service.history()->count() >= 2, 5000);
        GIT_TRY_VERIFY(!service.historyViewLoading(), 5000);
        QVERIFY(service.errorMessage().isEmpty());
    }

    QVERIFY(run(repository,
                {QStringLiteral("worktree"), QStringLiteral("add"), QStringLiteral("-b"),
                 QStringLiteral("linked"), worktree, QStringLiteral("main")})
                .succeeded());
    QVERIFY(QFileInfo(QDir(worktree).filePath(QStringLiteral(".git"))).isFile());
    QVERIFY(writeFile(QDir(worktree).filePath(QStringLiteral("tracked.txt")), "linked change\n"));

    GitRepositoryService linkedService;
    linkedService.setProjectPath(worktree);
    GIT_TRY_VERIFY(linkedService.available(), 5000);
    GIT_TRY_VERIFY(!linkedService.busy(), 5000);
    QCOMPARE(QDir::cleanPath(linkedService.repositoryRoot()), QDir::cleanPath(worktree));
    QCOMPARE(linkedService.currentBranch(), QStringLiteral("linked"));
    GIT_TRY_VERIFY(linkedService.history()->count() >= 2, 5000);
    GIT_TRY_COMPARE(
        linkedService.statusMap().value(QStringLiteral("tracked.txt")).toString(),
        QStringLiteral("modified"), 5000);
}

void GitRepositoryReadTest::refreshesRefsAndWorktreesOnRequest()
{
    QTemporaryDir root;
    QVERIFY(root.isValid());
    const QString repository = root.filePath(QStringLiteral("repository"));
    const QString linked = root.filePath(QStringLiteral("linked tree"));
    const QString plain = root.filePath(QStringLiteral("plain"));
    QVERIFY(QDir().mkpath(repository));
    QVERIFY(QDir().mkpath(plain));
    QVERIFY(initializeRepository(repository));
    QVERIFY(commitFile(repository, QStringLiteral("tracked.txt"), "first\n", QStringLiteral("first")));

    // Without a repository there is nothing to read and nothing to wait for.
    GitRepositoryService idle;
    idle.refreshRefs();
    QVERIFY(!idle.refsRefreshing());

    GitRepositoryService service;
    QSignalSpy refreshingSpy(&service, &GitRepositoryService::refsRefreshingChanged);
    service.setProjectPath(repository);
    GIT_TRY_COMPARE(service.history()->count(), 1, 5000);
    GIT_TRY_VERIFY(!service.busy(), 5000);
    // A repository without linked worktrees lists only itself.
    GIT_TRY_COMPARE(service.worktrees()->count(), 1, 5000);
    const QVariantMap only = service.worktrees()->itemAt(0);
    QVERIFY(only.value(QStringLiteral("main")).toBool());
    QVERIFY(only.value(QStringLiteral("current")).toBool());
    QCOMPARE(only.value(QStringLiteral("branch")).toString(), QStringLiteral("main"));
    // The reads that follow repository changes are not a manual refresh.
    QVERIFY(!service.refsRefreshing());
    QCOMPARE(refreshingSpy.count(), 0);

    QVERIFY(run(repository, {QStringLiteral("tag"), QStringLiteral("v1")}).succeeded());
    QVERIFY(
        run(repository,
            {QStringLiteral("worktree"), QStringLiteral("add"), QStringLiteral("-b"), QStringLiteral("topic"), linked})
            .succeeded());

    service.refreshRefs();
    QVERIFY(service.refsRefreshing());
    QCOMPARE(refreshingSpy.count(), 1);
    GIT_TRY_VERIFY(!service.refsRefreshing(), 5000);
    QCOMPARE(refreshingSpy.count(), 2);
    QVERIFY(service.errorMessage().isEmpty());
    QVERIFY(hasRef(service.branches(), QStringLiteral("tag"), QStringLiteral("v1")));
    QVERIFY(hasRef(service.branches(), QStringLiteral("local"), QStringLiteral("topic")));
    QCOMPARE(service.worktrees()->count(), 2);
    const QVariantMap mainTree = worktreeAt(service.worktrees(), repository);
    QVERIFY(mainTree.value(QStringLiteral("main")).toBool());
    QVERIFY(mainTree.value(QStringLiteral("current")).toBool());
    const QVariantMap linkedTree = worktreeAt(service.worktrees(), linked);
    QCOMPARE(linkedTree.value(QStringLiteral("branch")).toString(), QStringLiteral("topic"));
    QCOMPARE(linkedTree.value(QStringLiteral("name")).toString(), QStringLiteral("linked tree"));
    QVERIFY(!linkedTree.value(QStringLiteral("main")).toBool());
    QVERIFY(!linkedTree.value(QStringLiteral("current")).toBool());

    // A service inside the linked worktree marks that one as current.
    GitRepositoryService linkedService;
    linkedService.setProjectPath(linked);
    GIT_TRY_COMPARE(linkedService.worktrees()->count(), 2, 5000);
    QVERIFY(worktreeAt(linkedService.worktrees(), linked).value(QStringLiteral("current")).toBool());
    QVERIFY(!worktreeAt(linkedService.worktrees(), repository).value(QStringLiteral("current")).toBool());

    // Switching repository ends the refresh and drops what it was reading:
    // the late results of the old repository must not come back.
    QVERIFY(run(repository, {QStringLiteral("tag"), QStringLiteral("v2")}).succeeded());
    service.refreshRefs();
    QVERIFY(service.refsRefreshing());
    service.setProjectPath(plain);
    QVERIFY(!service.refsRefreshing());
    QCOMPARE(service.worktrees()->count(), 0);
    QCOMPARE(service.branches()->count(), 0);
    QTest::qWait(500);
    QVERIFY(!service.refsRefreshing());
    QCOMPARE(service.worktrees()->count(), 0);
    QCOMPARE(service.branches()->count(), 0);
}

void GitRepositoryReadTest::probesWorktreeChangesInBackground()
{
    QTemporaryDir root;
    QVERIFY(root.isValid());
    const QString repository = root.filePath(QStringLiteral("repository"));
    const QString linked = root.filePath(QStringLiteral("linked tree"));
    const QString plain = root.filePath(QStringLiteral("plain"));
    QVERIFY(QDir().mkpath(repository));
    QVERIFY(QDir().mkpath(plain));
    QVERIFY(initializeRepository(repository));
    QVERIFY(commitFile(repository, QStringLiteral("tracked.txt"), "first\n", QStringLiteral("first")));
    QVERIFY(
        run(repository,
            {QStringLiteral("worktree"), QStringLiteral("add"), QStringLiteral("-b"), QStringLiteral("topic"), linked})
            .succeeded());
    // topic tracks main, with one commit of its own (not pushed) and one of
    // main's it lacks (not pulled).
    QVERIFY(
        run(repository, {QStringLiteral("branch"), QStringLiteral("--set-upstream-to=main"), QStringLiteral("topic")})
            .succeeded());
    QVERIFY(commitFile(linked, QStringLiteral("topic.txt"), "topic\n", QStringLiteral("topic")));
    QVERIFY(commitFile(repository, QStringLiteral("main.txt"), "main\n", QStringLiteral("main")));
    // gone tracks a branch deleted since; solo tracks nothing.
    QVERIFY(run(repository, {QStringLiteral("branch"), QStringLiteral("doomed")}).succeeded());
    QVERIFY(run(repository,
                {QStringLiteral("branch"), QStringLiteral("--track"), QStringLiteral("gone"), QStringLiteral("doomed")})
                .succeeded());
    QVERIFY(run(repository, {QStringLiteral("branch"), QStringLiteral("-D"), QStringLiteral("doomed")}).succeeded());
    QVERIFY(run(repository, {QStringLiteral("branch"), QStringLiteral("solo")}).succeeded());
    // The linked worktree edits a tracked file; the main one only gains an
    // untracked file, which the probe leaves out.
    QVERIFY(writeFile(QDir(linked).filePath(QStringLiteral("tracked.txt")), "edited\n"));
    QVERIFY(writeFile(QDir(repository).filePath(QStringLiteral("untracked.txt")), "new\n"));

    GitRepositoryService service;
    service.setProjectPath(repository);
    GIT_TRY_COMPARE(service.worktrees()->count(), 2, 5000);
    GIT_TRY_VERIFY(hasRef(service.branches(), QStringLiteral("local"), QStringLiteral("solo")), 5000);

    // Ahead/behind compare with the upstream ref as it is locally.
    const QVariantMap topic = refRow(service.branches(), QStringLiteral("local"), QStringLiteral("topic"));
    QVERIFY(topic.value(QStringLiteral("hasUpstream")).toBool());
    QVERIFY(!topic.value(QStringLiteral("upstreamGone")).toBool());
    QCOMPARE(topic.value(QStringLiteral("ahead")).toInt(), 1);
    QCOMPARE(topic.value(QStringLiteral("behind")).toInt(), 1);
    const QVariantMap gone = refRow(service.branches(), QStringLiteral("local"), QStringLiteral("gone"));
    QVERIFY(gone.value(QStringLiteral("hasUpstream")).toBool());
    QVERIFY(gone.value(QStringLiteral("upstreamGone")).toBool());
    QVERIFY(!refRow(service.branches(), QStringLiteral("local"), QStringLiteral("solo"))
                 .value(QStringLiteral("hasUpstream"))
                 .toBool());

    // The probe returns at once and answers as each check lands.
    const QString mainPath = worktreeAt(service.worktrees(), repository).value(QStringLiteral("path")).toString();
    const QString linkedPath = worktreeAt(service.worktrees(), linked).value(QStringLiteral("path")).toString();
    service.probeWorktreeChanges();
    QVERIFY(service.worktreeChanges().isEmpty());
    GIT_TRY_COMPARE(service.worktreeChanges().size(), 2, 5000);
    QVERIFY(service.worktreeChanges().value(linkedPath).toBool());
    QVERIFY(!service.worktreeChanges().value(mainPath).toBool());
    // Read only: the edit is still unstaged in its worktree.
    QVERIFY(run(linked, {QStringLiteral("diff"), QStringLiteral("--cached"), QStringLiteral("--quiet")}).succeeded());
    QVERIFY(!run(linked, {QStringLiteral("diff"), QStringLiteral("--quiet")}).succeeded());

    // A newer round supersedes the running one and still converges.
    QVERIFY(run(linked, {QStringLiteral("checkout"), QStringLiteral("--"), QStringLiteral("tracked.txt")}).succeeded());
    service.probeWorktreeChanges();
    service.probeWorktreeChanges();
    GIT_TRY_VERIFY(!service.worktreeChanges().value(linkedPath, true).toBool(), 5000);
    QCOMPARE(service.worktreeChanges().size(), 2);
    QVERIFY(service.errorMessage().isEmpty());

    // One round checks at most kGitWorktreeProbeLimit worktrees; the manual
    // refresh probes the list it re-read.
    QVERIFY(addDetachedWorktrees(repository, root, kGitWorktreeProbeLimit));
    service.refreshRefs();
    GIT_TRY_COMPARE(service.worktrees()->count(), kGitWorktreeProbeLimit + 2, 5000);
    GIT_TRY_COMPARE(service.worktreeChanges().size(), kGitWorktreeProbeLimit, 20000);
    QTest::qWait(300);
    QCOMPARE(service.worktreeChanges().size(), kGitWorktreeProbeLimit);

    // A repository switch drops the round: none of its results come back.
    service.probeWorktreeChanges();
    service.setProjectPath(plain);
    QVERIFY(service.worktreeChanges().isEmpty());
    QTest::qWait(500);
    QVERIFY(service.worktreeChanges().isEmpty());
}

void GitRepositoryReadTest::pollIntervalFollowsIdleFlag()
{
    // An idle user backs the status poll off; activity restores it.
    GitRepositoryService service;
    const int active = service.pollIntervalMs();
    QVERIFY(active > 0);
    service.setIdle(true);
    QVERIFY(service.pollIntervalMs() > active);
    service.setIdle(false);
    QCOMPARE(service.pollIntervalMs(), active);
}

void GitRepositoryReadTest::pollRunsStatusOnlyAfterWorkTreeChanges()
{
    QTemporaryDir root;
    QVERIFY(root.isValid());
    const QString repository = root.filePath(QStringLiteral("repository"));
    QVERIFY(QDir().mkpath(repository));
    QVERIFY(initializeRepository(repository));
    QVERIFY(commitFile(repository, QStringLiteral("tracked.txt"), "first\n", QStringLiteral("first")));

    GitRepositoryService service;
    service.setProjectPath(repository);
    GIT_TRY_COMPARE(service.history()->count(), 1, 5000);
    GIT_TRY_VERIFY(!service.busy(), 5000);
    service.setPollIntervalForTesting(50);

    // A quiet working tree: the poll keeps ticking without starting git.
    QTest::qWait(300);
    const int quietRuns = gitStatusRuns();
    QTest::qWait(600);
    QCOMPARE(gitStatusRuns(), quietRuns);

    // An edit only the working tree sees is picked up by the next poll.
    QVERIFY(writeFile(QDir(repository).filePath(QStringLiteral("tracked.txt")), "edited\n"));
    GIT_TRY_COMPARE(service.statusMap().value(QStringLiteral("tracked.txt")).toString(), QStringLiteral("modified"),
                    3000);
    QVERIFY(gitStatusRuns() > quietRuns);
}

void GitRepositoryReadTest::baseContentKeepsRefreshChain()
{
    QTemporaryDir root;
    QVERIFY(root.isValid());
    const QString repository = root.filePath(QStringLiteral("repository"));
    QVERIFY(QDir().mkpath(repository));
    QVERIFY(initializeRepository(repository));
    const QString tracked = QDir(repository).filePath(QStringLiteral("tracked.txt"));
    QVERIFY(writeFile(tracked, "first\n"));
    QVERIFY(run(repository, {QStringLiteral("add"), QStringLiteral("tracked.txt")}).succeeded());
    QVERIFY(run(repository,
                {QStringLiteral("commit"), QStringLiteral("-m"), QStringLiteral("initial")})
                .succeeded());

    // An open editor asks for its file's HEAD text while the first refresh
    // is still running; the refresh must still go on to branches and history.
    GitRepositoryService service;
    bool requested = false;
    requestBaseContentWhileLoading(service, &requested);
    QSignalSpy baseSpy(&service, &GitRepositoryService::baseContentReady);
    service.setProjectPath(repository);
    GIT_TRY_VERIFY(requested, 5000);
    GIT_TRY_COMPARE(baseSpy.count(), 1, 5000);
    QCOMPARE(baseSpy.at(0).at(1).toString(), QStringLiteral("first\n"));
    GIT_TRY_COMPARE(service.history()->count(), 1, 5000);
}

void GitRepositoryReadTest::loadsHistoryOfBranchNamedLikeDirectory()
{
    QTemporaryDir root;
    QVERIFY(root.isValid());
    const QString repository = root.filePath(QStringLiteral("repository"));
    QVERIFY(QDir().mkpath(repository));
    QVERIFY(initializeRepository(repository));
    QVERIFY(QDir(repository).mkpath(QStringLiteral("docs")));
    QVERIFY(writeFile(QDir(repository).filePath(QStringLiteral("docs/readme.txt")), "doc\n"));
    QVERIFY(run(repository, {QStringLiteral("add"), QStringLiteral(".")}).succeeded());
    QVERIFY(run(repository,
                {QStringLiteral("commit"), QStringLiteral("-m"), QStringLiteral("initial")})
                .succeeded());
    // "docs" is now both a branch and a worktree directory.
    QVERIFY(run(repository, {QStringLiteral("switch"), QStringLiteral("-c"),
                             QStringLiteral("docs")}).succeeded());

    GitRepositoryService service;
    service.setProjectPath(repository);
    GIT_TRY_COMPARE(service.currentBranch(), QStringLiteral("docs"), 5000);
    GIT_TRY_COMPARE(service.history()->count(), 1, 5000);
    GIT_TRY_VERIFY(!service.historyViewLoading(), 5000);
    QVERIFY(service.errorMessage().isEmpty());

    // An amend replaces the selected commit: the selection moves to the new
    // head instead of pointing at a commit the history no longer shows.
    const QString original = service.history()->itemAt(0).value(QStringLiteral("hash")).toString();
    GIT_TRY_COMPARE(service.selectedCommit(), original, 5000);
    QVERIFY(run(repository, {QStringLiteral("commit"), QStringLiteral("--amend"),
                             QStringLiteral("-m"), QStringLiteral("amended")}).succeeded());
    service.refresh();
    GIT_TRY_COMPARE(
        service.selectedCommitDetails().value(QStringLiteral("subject")).toString(),
        QStringLiteral("amended"), 5000);
    QVERIFY(service.selectedCommit() != original);
}

void GitRepositoryReadTest::selectsCommitInSha256Repository()
{
    QTemporaryDir root;
    QVERIFY(root.isValid());
    const QString repository = root.filePath(QStringLiteral("repository"));
    QVERIFY(QDir().mkpath(repository));
    QVERIFY(run(repository,
                {QStringLiteral("init"), QStringLiteral("--object-format=sha256"), QStringLiteral("-b"),
                 QStringLiteral("main")})
                .succeeded());
    QVERIFY(initializeRepository(repository));
    QVERIFY(writeFile(QDir(repository).filePath(QStringLiteral("tracked.txt")), "first\n"));
    QVERIFY(run(repository, {QStringLiteral("add"), QStringLiteral("tracked.txt")}).succeeded());
    QVERIFY(run(repository,
                {QStringLiteral("commit"), QStringLiteral("-m"), QStringLiteral("initial")})
                .succeeded());

    GitRepositoryService service;
    service.setProjectPath(repository);
    GIT_TRY_COMPARE(service.history()->count(), 1, 5000);
    const QString hash = service.history()->itemAt(0).value(QStringLiteral("hash")).toString();
    QCOMPARE(hash.size(), 64);
    service.selectHistoryCommit(hash);
    GIT_TRY_VERIFY(service.commitDiffFiles()->count() == 1, 5000);
    QVERIFY(service.errorMessage().isEmpty());
}

void GitRepositoryReadTest::servesDiffBodiesBeforeFirstCommit()
{
    QTemporaryDir root;
    QVERIFY(root.isValid());
    const QString repository = root.filePath(QStringLiteral("repository"));
    QVERIFY(QDir().mkpath(repository));
    QVERIFY(initializeRepository(repository));
    const QDir dir(repository);
    QVERIFY(writeFile(dir.filePath(QStringLiteral("staged.txt")), "one\ntwo\n"));
    QVERIFY(run(repository, {QStringLiteral("add"), QStringLiteral("staged.txt")}).succeeded());
    // Staged, then edited again: neither the index nor the worktree diff
    // holds the whole file.
    QVERIFY(writeFile(dir.filePath(QStringLiteral("mixed.txt")), "one\n"));
    QVERIFY(run(repository, {QStringLiteral("add"), QStringLiteral("mixed.txt")}).succeeded());
    QVERIFY(writeFile(dir.filePath(QStringLiteral("mixed.txt")), "uno\ntwo\n"));
    QVERIFY(writeFile(dir.filePath(QStringLiteral("binary.dat")), QByteArray("a\0b", 3)));
    QVERIFY(writeFile(dir.filePath(QStringLiteral("crlf.txt")), "x\r\ny\r\n"));

    GitRepositoryService service;
    service.setProjectPath(repository);
    GIT_TRY_VERIFY(service.available(), 5000);
    GIT_TRY_VERIFY(!service.busy(), 5000);
    GIT_TRY_COMPARE(service.commitChanges()->count(), 4, 5000);
    // Totals count each file from disk: summing the index and worktree diffs
    // would make mixed.txt +3 -1.
    GIT_TRY_COMPARE(service.diffAdditions(), 6, 5000);
    QCOMPARE(service.diffDeletions(), 0);
    const auto body = [&service](const QString &scope, const QString &path) {
        return service.diffLines(scope, path).toMap();
    };
    const auto lines = [&body](const QString &scope, const QString &path) {
        return body(scope, path).value(QStringLiteral("lines")).toList();
    };

    // Without a HEAD every listed file is new in full, so each body is the
    // working-tree file, staged or not.
    const QString changes = QStringLiteral("changes");
    service.requestDiffLines(changes, {QStringLiteral("staged.txt"), QStringLiteral("mixed.txt"),
                                       QStringLiteral("binary.dat"), QStringLiteral("crlf.txt")});
    GIT_TRY_VERIFY(!body(changes, QStringLiteral("staged.txt")).isEmpty()
                                 && !body(changes, QStringLiteral("mixed.txt")).isEmpty()
                                 && !body(changes, QStringLiteral("binary.dat")).isEmpty()
                                 && !body(changes, QStringLiteral("crlf.txt")).isEmpty(),
                             5000);
    QCOMPARE(body(changes, QStringLiteral("staged.txt")).value(QStringLiteral("error")).toString(),
             QString());
    QCOMPARE(lines(changes, QStringLiteral("staged.txt")).size(), 2);
    const QVariantList mixed = lines(changes, QStringLiteral("mixed.txt"));
    QCOMPARE(mixed.size(), 2);
    QCOMPARE(mixed.at(1).toMap().value(QStringLiteral("kind")).toString(), QStringLiteral("add"));
    QCOMPARE(mixed.at(1).toMap().value(QStringLiteral("newText")).toString(), QStringLiteral("two"));
    QCOMPARE(
        rowWithPath(service.commitChanges(), QStringLiteral("mixed.txt")).value(QStringLiteral("additions")).toInt(),
        2);
    QVERIFY(body(changes, QStringLiteral("binary.dat")).value(QStringLiteral("binary")).toBool());
    QVERIFY(lines(changes, QStringLiteral("binary.dat")).isEmpty());
    const QVariantList crlf = lines(changes, QStringLiteral("crlf.txt"));
    QCOMPARE(crlf.size(), 2);
    QCOMPARE(crlf.at(0).toMap().value(QStringLiteral("newText")).toString(), QStringLiteral("x"));
    // The unborn branch has no history to load, and that is not an error.
    QCOMPARE(service.history()->count(), 0);
    QVERIFY2(service.errorMessage().isEmpty(), qPrintable(service.errorMessage()));

    // The root commit has no parent to diff against: its files come from
    // `git show` alone.
    QVERIFY(run(repository, {QStringLiteral("add"), QStringLiteral("-A")}).succeeded());
    QVERIFY(run(repository, {QStringLiteral("commit"), QStringLiteral("-m"), QStringLiteral("root")})
                .succeeded());
    service.refresh();
    GIT_TRY_COMPARE(service.history()->count(), 1, 5000);
    GIT_TRY_VERIFY(!service.busy(), 5000);
    service.selectHistoryCommit(service.history()->itemAt(0).value(QStringLiteral("hash")).toString());
    service.refresh();
    GIT_TRY_VERIFY(!service.commitViewLoading(), 5000);
    GIT_TRY_COMPARE(service.commitDiffFiles()->count(), 4, 5000);
    const QString commit = QStringLiteral("commit");
    service.requestDiffLines(commit, {QStringLiteral("staged.txt")});
    GIT_TRY_COMPARE(lines(commit, QStringLiteral("staged.txt")).size(), 2, 5000);
    QVERIFY(service.errorMessage().isEmpty());

    // A batch whose diff passes the 16 MiB display limit is retried file by
    // file, so only the oversized file is refused. Git writes this diff and
    // exits before the kill lands: the limit must hold anyway.
    QVERIFY(writeFile(dir.filePath(QStringLiteral("staged.txt")), "one\nthree\n"));
    QVERIFY(writeFile(dir.filePath(QStringLiteral("crlf.txt")),
                      QByteArray(17 * 1024 * 1024 / 8, 'x').append('\n').repeated(8)));
    service.refresh();
    GIT_TRY_VERIFY(!service.busy(), 5000);
    service.requestDiffLines(changes, {QStringLiteral("staged.txt"), QStringLiteral("crlf.txt")});
    GIT_TRY_VERIFY(
        body(changes, QStringLiteral("crlf.txt")).value(QStringLiteral("tooLarge")).toBool(), 5000);
    GIT_TRY_VERIFY(lines(changes, QStringLiteral("staged.txt")).size() >= 2, 5000);
    QVERIFY(lines(changes, QStringLiteral("crlf.txt")).isEmpty());
}

void GitRepositoryReadTest::pagesHistoryAndPrependsNewCommits()
{
    QTemporaryDir root;
    QVERIFY(root.isValid());
    const QString repository = root.filePath(QStringLiteral("repository"));
    QVERIFY(QDir().mkpath(repository));
    QVERIFY(initializeRepository(repository));
    const int page = kGitHistoryPageSize;
    const int total = page + 20;
    QVERIFY(importCommits(repository, numberedCommits(total)));

    // The first read stops at one page.
    GitRepositoryService service;
    service.setProjectPath(repository);
    GIT_TRY_COMPARE(service.history()->count(), page, 10000);
    GIT_TRY_VERIFY(!service.busy(), 10000);
    QVERIFY(service.historyHasMore());
    QCOMPARE(subjectAt(service, 0), QStringLiteral("commit-%1").arg(total - 1));

    // The next page continues below it, and the history is then complete.
    service.loadMoreHistory();
    GIT_TRY_COMPARE(service.history()->count(), total, 10000);
    GIT_TRY_VERIFY(!service.busy(), 10000);
    QVERIFY(!service.historyHasMore());
    QCOMPARE(subjectAt(service, page), QStringLiteral("commit-%1").arg(total - page - 1));
    QCOMPARE(subjectAt(service, total - 1), QStringLiteral("commit-0"));
    service.loadMoreHistory();
    QVERIFY(!service.busy());

    // A new commit on top is inserted above the loaded rows; nothing reloads.
    QSignalSpy insertedSpy(service.history(), &QAbstractItemModel::rowsInserted);
    QSignalSpy removedSpy(service.history(), &QAbstractItemModel::rowsRemoved);
    QSignalSpy resetSpy(service.history(), &QAbstractItemModel::modelReset);
    QVERIFY(commitFile(repository, QStringLiteral("file.txt"), "newest\n", QStringLiteral("newest")));
    service.refresh();
    GIT_TRY_COMPARE(service.history()->count(), total + 1, 10000);
    GIT_TRY_VERIFY(!service.busy(), 10000);
    QCOMPARE(subjectAt(service, 0), QStringLiteral("newest"));
    QCOMPARE(subjectAt(service, total), QStringLiteral("commit-0"));
    QCOMPARE(insertedSpy.count(), 1);
    QCOMPARE(insertedSpy.at(0).at(1).toInt(), 0);
    QCOMPARE(insertedSpy.at(0).at(2).toInt(), 0);
    QCOMPARE(removedSpy.count(), 0);
    QCOMPARE(resetSpy.count(), 0);
    // The branch label moved off the old head row.
    QVERIFY(headDecorated(service.history(), 0));
    QVERIFY(!headDecorated(service.history(), 1));

    // A rewritten head is not above the old one: the history starts over
    // from its first page.
    QVERIFY(
        run(repository,
            {QStringLiteral("commit"), QStringLiteral("--amend"), QStringLiteral("-m"), QStringLiteral("rewritten")})
            .succeeded());
    service.refresh();
    GIT_TRY_COMPARE(subjectAt(service, 0), QStringLiteral("rewritten"), 10000);
    GIT_TRY_COMPARE(service.history()->count(), page, 10000);
    QVERIFY(service.historyHasMore());
}

void GitRepositoryReadTest::backgroundProjectPausesUntilShown()
{
    QTemporaryDir root;
    QVERIFY(root.isValid());
    const QString repository = root.filePath(QStringLiteral("repository"));
    QVERIFY(QDir().mkpath(repository));
    QVERIFY(initializeRepository(repository));
    QVERIFY(commitFile(repository, QStringLiteral("tracked.txt"), "first\n", QStringLiteral("first")));

    GitRepositoryService service;
    service.setProjectPath(repository);
    GIT_TRY_COMPARE(service.history()->count(), 1, 5000);
    GIT_TRY_VERIFY(!service.busy(), 5000);
    QVERIFY(service.polling());

    service.setForeground(false);
    QVERIFY(!service.polling());
    // Neither the watcher (a new commit) nor the poll (an edit) refreshes a
    // background project.
    QVERIFY(commitFile(repository, QStringLiteral("tracked.txt"), "second\n", QStringLiteral("second")));
    QVERIFY(writeFile(QDir(repository).filePath(QStringLiteral("tracked.txt")), "edited\n"));
    QTest::qWait(800);
    QCOMPARE(service.history()->count(), 1);
    QVERIFY(service.statusMap().isEmpty());

    // Showing it again refreshes at once, long before the next poll is due.
    service.setForeground(true);
    QVERIFY(service.polling());
    GIT_TRY_COMPARE(service.history()->count(), 2, 3000);
    GIT_TRY_COMPARE(service.statusMap().value(QStringLiteral("tracked.txt")).toString(), QStringLiteral("modified"),
                    3000);

    // A project that starts in the background discovers its repository
    // only once it is shown.
    GitRepositoryService hidden;
    QStringList started;
    connect(&hidden, &GitRepositoryService::taskStarted, &hidden,
            [&started](const QString &operation) { started.append(operation); });
    hidden.setForeground(false);
    hidden.setProjectPath(repository);
    QTest::qWait(200);
    QVERIFY(started.isEmpty());
    QVERIFY(!hidden.available());
    hidden.setForeground(true);
    GIT_TRY_VERIFY(hidden.available(), 5000);
    GIT_TRY_COMPARE(hidden.history()->count(), 2, 5000);
}

void GitRepositoryReadTest::ordinaryDirectoryWaitsForRepository()
{
    QTemporaryDir root;
    QVERIFY(root.isValid());
    const QString plain = root.filePath(QStringLiteral("plain"));
    QVERIFY(QDir().mkpath(plain));

    GitRepositoryService service;
    QStringList started;
    connect(&service, &GitRepositoryService::taskStarted, &service,
            [&started](const QString &operation) { started.append(operation); });
    service.setProjectPath(plain);
    GIT_TRY_VERIFY(started.count(QStringLiteral("discover")) == 1 && !service.busy(), 5000);
    QVERIFY(!service.available());
    QVERIFY(service.errorMessage().isEmpty());
    // No poll re-runs rev-parse on an ordinary directory; files coming and
    // going in it do not either.
    QVERIFY(!service.polling());
    QVERIFY(writeFile(QDir(plain).filePath(QStringLiteral("notes.txt")), "x\n"));
    QTest::qWait(300);
    QCOMPARE(started.count(QStringLiteral("discover")), 1);

    // A repository created there is found without a restart.
    QVERIFY(initializeRepository(plain));
    GIT_TRY_VERIFY(service.available(), 5000);
    QCOMPARE(QDir::cleanPath(service.repositoryRoot()), QDir::cleanPath(plain));
    GIT_TRY_VERIFY(service.polling(), 5000);
}

void GitRepositoryReadTest::dropsDiffBodyOfPreviousSelection()
{
    QTemporaryDir root;
    QVERIFY(root.isValid());
    const QString repository = root.filePath(QStringLiteral("repository"));
    QVERIFY(QDir().mkpath(repository));
    QVERIFY(initializeRepository(repository));
    const QString file = QStringLiteral("file.txt");
    QVERIFY(commitFile(repository, file, "one\n", QStringLiteral("one")));
    QVERIFY(commitFile(repository, file, "two\n", QStringLiteral("two")));
    const QString second = revParse(repository, QStringLiteral("HEAD"));
    QVERIFY(commitFile(repository, file, "three\n", QStringLiteral("three")));
    const QString third = revParse(repository, QStringLiteral("HEAD"));

    GitRepositoryService service;
    service.setProjectPath(repository);
    GIT_TRY_COMPARE(service.history()->count(), 3, 5000);
    GIT_TRY_VERIFY(!service.busy(), 5000);
    const QString commit = QStringLiteral("commit");
    service.selectHistoryCommit(second);
    GIT_TRY_COMPARE(service.commitDiffFiles()->count(), 1, 5000);
    service.requestDiffLines(commit, {file});
    // Another commit is selected before that body arrives: the late body
    // must not be served as the new commit's diff of the same file.
    service.selectHistoryCommit(third);
    GIT_TRY_COMPARE(service.commitDiffFiles()->count(), 1, 5000);
    service.requestDiffLines(commit, {file});
    GIT_TRY_VERIFY(!service.diffLines(commit, file).isNull(), 5000);
    QCOMPARE(addedTexts(service.diffLines(commit, file)), QStringList{QStringLiteral("three")});
}

QTEST_MAIN(GitRepositoryReadTest)
#include "git_repository_read_test.moc"
