#include "git/git_worktree_commands.h"
#include "git/git_worktree_layout.h"
#include "git_test_support.h"

#include <QDir>
#include <QFileInfo>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>

using span::tests::git::initializeRepository;
using span::tests::git::run;
using span::tests::git::writeFile;

class GitWorktreeTest final : public QObject {
    Q_OBJECT

private slots:
    void initTestCase();
    void branchNameRules_data();
    void branchNameRules();
    void parsesBranchList();
    void createsListsAndDeletesWorktree();
    void ordinaryFolderHasNoWorktreeRelation();
};

namespace {

QString canonical(const QString &path)
{
    return QFileInfo(path).canonicalFilePath();
}

bool commitFile(const QString &repository, const QString &path, const QByteArray &contents)
{
    return writeFile(QDir(repository).filePath(path), contents)
        && run(repository, {QStringLiteral("add"), QStringLiteral("--"), path}).succeeded()
        && run(repository, {QStringLiteral("commit"), QStringLiteral("-m"), path}).succeeded();
}

struct Outcome {
    bool done = false;
    bool ok = false;
    QString error;
};

} // namespace

void GitWorktreeTest::initTestCase()
{
    if (!run(QDir::currentPath(), {QStringLiteral("--version")}).succeeded()) QSKIP("git is not available");
    // Without a ceiling, git would find this checkout from a plain folder.
    qputenv("GIT_CEILING_DIRECTORIES", QDir::toNativeSeparators(QDir::tempPath()).toLocal8Bit());
}

void GitWorktreeTest::branchNameRules_data()
{
    QTest::addColumn<QString>("name");
    QTest::addColumn<bool>("valid");
    QTest::newRow("plain") << QStringLiteral("feature") << true;
    QTest::newRow("nested") << QStringLiteral("feature/login") << true;
    QTest::newRow("non-ascii") << QStringLiteral("修复-登录") << true;
    QTest::newRow("empty") << QString() << false;
    QTest::newRow("space") << QStringLiteral("my branch") << false;
    QTest::newRow("dot-dot") << QStringLiteral("a..b") << false;
    QTest::newRow("leading dash") << QStringLiteral("-x") << false;
    QTest::newRow("trailing slash") << QStringLiteral("a/") << false;
    QTest::newRow("lock suffix") << QStringLiteral("a.lock") << false;
    QTest::newRow("component dot") << QStringLiteral("a/.b") << false;
    QTest::newRow("reflog syntax") << QStringLiteral("a@{1}") << false;
    QTest::newRow("at only") << QStringLiteral("@") << false;
    QTest::newRow("glob") << QStringLiteral("a*") << false;
    QTest::newRow("colon") << QStringLiteral("a:b") << false;
    QTest::newRow("backslash") << QStringLiteral("a\\b") << false;
    QTest::newRow("double slash") << QStringLiteral("a//b") << false;
}

void GitWorktreeTest::branchNameRules()
{
    QFETCH(QString, name);
    QFETCH(bool, valid);
    QCOMPARE(span::git::isValidBranchName(name), valid);
}

void GitWorktreeTest::parsesBranchList()
{
    const QByteArray output = " \trefs/heads/feature/login\n"
                              "*\trefs/heads/main\n"
                              " \trefs/remotes/origin/HEAD\n"
                              " \trefs/remotes/origin/main\n";
    QString current;
    const QStringList branches = GitWorktreeCommands::parseBranches(output, &current);
    QCOMPARE(branches,
             (QStringList{QStringLiteral("feature/login"), QStringLiteral("main"), QStringLiteral("origin/main")}));
    QCOMPARE(current, QStringLiteral("main"));
}

void GitWorktreeTest::createsListsAndDeletesWorktree()
{
    QTemporaryDir root;
    QVERIFY(root.isValid());
    const QString repository = root.filePath(QStringLiteral("repository"));
    const QString worktree = root.filePath(QStringLiteral("worktrees/repository/feature-login"));
    QVERIFY(QDir().mkpath(repository));
    QVERIFY(initializeRepository(repository));
    QVERIFY(commitFile(repository, QStringLiteral("tracked.txt"), "first\n"));

    GitWorktreeCommands commands;
    QSignalSpy started(&commands, &GitWorktreeCommands::progressStarted);
    QSignalSpy finished(&commands, &GitWorktreeCommands::progressFinished);

    // The create dialog's branch read.
    bool listed = false;
    QStringList branches;
    QString current;
    commands.listBranches(repository, [&](const QStringList &names, const QString &head) {
        listed = true;
        branches = names;
        current = head;
    });
    GIT_TRY_VERIFY(listed, 10000);
    QCOMPARE(branches, QStringList{QStringLiteral("main")});
    QCOMPARE(current, QStringLiteral("main"));

    // Unsafe arguments never start git.
    QVERIFY(!commands.add(repository, QStringLiteral("bad name"), QStringLiteral("main"), worktree, {}));
    QVERIFY(!commands.add(repository, QStringLiteral("feature/login"), QStringLiteral("main"),
                          QStringLiteral("relative/path"), {}));
    QCOMPARE(started.count(), 0);

    Outcome added;
    QVERIFY(commands.add(repository, QStringLiteral("feature/login"), QStringLiteral("main"), worktree,
                         [&](bool ok, const QString &error) { added = {true, ok, error}; }));
    QVERIFY(commands.busy());
    // One write at a time.
    QVERIFY(!commands.remove(repository, worktree, false, {}));
    GIT_TRY_VERIFY(added.done, 20000);
    QVERIFY2(added.ok, qPrintable(added.error));
    QVERIFY(!commands.busy());
    QCOMPARE(started.count(), 1);
    QCOMPARE(finished.count(), 1);
    QCOMPARE(finished.first().at(0).value<span::core::OperationOutcome>(), span::core::OperationOutcome::Succeeded);

    // The folder layout git wrote, read without git.
    QVERIFY(span::git::isWorktreeRoot(worktree));
    QVERIFY(span::git::isWorktreeRoot(repository));
    QCOMPARE(canonical(span::git::mainWorktreeOf(worktree)), canonical(repository));
    QVERIFY(span::git::mainWorktreeOf(repository).isEmpty());
    const QList<span::git::LinkedWorktree> linked = span::git::linkedWorktreesOf(repository);
    QCOMPARE(linked.size(), 1);
    QCOMPARE(canonical(linked.first().path), canonical(worktree));
    QCOMPARE(linked.first().branch, QStringLiteral("feature/login"));
    // The project list's branch label, for both kinds of worktree.
    QCOMPARE(span::git::headLabelOf(repository), QStringLiteral("main"));
    QCOMPARE(span::git::headLabelOf(worktree), QStringLiteral("feature/login"));
    // A detached HEAD shows its short commit.
    const QString commit =
        QString::fromUtf8(run(repository, {QStringLiteral("rev-parse"), QStringLiteral("HEAD")}).stdoutBytes).trimmed();
    QVERIFY(run(worktree, {QStringLiteral("checkout"), QStringLiteral("--detach")}).succeeded());
    QCOMPARE(span::git::headLabelOf(worktree), commit.left(7));

    // A worktree with an untracked file: git refuses until forced.
    QVERIFY(writeFile(QDir(worktree).filePath(QStringLiteral("scratch.txt")), "draft\n"));
    Outcome refused;
    QVERIFY(commands.remove(repository, worktree, false,
                            [&](bool ok, const QString &error) { refused = {true, ok, error}; }));
    GIT_TRY_VERIFY(refused.done, 20000);
    QVERIFY(!refused.ok);
    QVERIFY(!refused.error.isEmpty());
    QVERIFY(QFileInfo::exists(worktree));
    QCOMPARE(finished.last().at(0).value<span::core::OperationOutcome>(), span::core::OperationOutcome::Failed);

    Outcome forced;
    QVERIFY(commands.remove(repository, worktree, true,
                            [&](bool ok, const QString &error) { forced = {true, ok, error}; }));
    GIT_TRY_VERIFY(forced.done, 20000);
    QVERIFY2(forced.ok, qPrintable(forced.error));
    QVERIFY(!QFileInfo::exists(worktree));
    QVERIFY(span::git::linkedWorktreesOf(repository).isEmpty());
    // The branch stays.
    QVERIFY(run(repository,
                {QStringLiteral("rev-parse"), QStringLiteral("--verify"), QStringLiteral("refs/heads/feature/login")})
                .succeeded());
}

void GitWorktreeTest::ordinaryFolderHasNoWorktreeRelation()
{
    QTemporaryDir root;
    QVERIFY(root.isValid());
    QVERIFY(!span::git::isWorktreeRoot(root.path()));
    QVERIFY(span::git::mainWorktreeOf(root.path()).isEmpty());
    QVERIFY(span::git::linkedWorktreesOf(root.path()).isEmpty());
    QVERIFY(span::git::headLabelOf(root.path()).isEmpty());
}

QTEST_GUILESS_MAIN(GitWorktreeTest)

#include "git_worktree_test.moc"
