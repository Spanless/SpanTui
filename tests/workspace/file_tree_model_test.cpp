#include "workspace/file_tree_model.h"

#include <QAbstractItemModel>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSignalSpy>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTest>

#include <windows.h>

#include <algorithm>

class FileTreeModelTest final : public QObject {
    Q_OBJECT

private slots:
    void exposesExpandedProjectRoot();
    void createsItemsInRootNode();
    void protectsRootFromRenameAndDelete();
    void rejectsInvalidNames();
    void preservesExpansionAfterCreate();
    void exposesGitAggregationAndCategories();
    void ignoredGitPathsDimWithoutPromotingOntoParents();
    void buildsSafeCommandsForWindowsScripts_data();
    void buildsSafeCommandsForWindowsScripts();
    void refusesToRunNonScriptsAndScriptNamedFolders();
    void createsSiblingsBesideSelectedFile();
    void initTestCase();
    void renamesFileInsideProject();
    void rejectsRenameEscapeAndCollision();
    void deletesToInternalTrashAndUndoes();
    void rejectsNameEquivalenceClasses_data();
    void rejectsNameEquivalenceClasses();
    void pathsOutsideRootDoNotMutateDisk();
    void undoWithEmptyStackReportsError();
    void deletesDirectoryTreeAndUndoes();
    void renameKeepsDirectoryWhenItemInsideIsLocked();
    void copyMoveDeletesTreeWithHiddenFilesAndUndoes();
    void copyMoveLeavesSourceWholeWhenItemInsideIsLocked();
    void undoFailsWhenOriginalFolderBecameAFile();
    void acceptsUnicodeFileName();
    void reportsBusyAroundRootLoad();
    void expandingDirectoryReportsRowLoadingWithoutClearingTree();
    void discardsStaleRootLoad();
    void collapseDuringLoadDiscardsStaleChildren();
    void collapsingSmallDirectoryKeepsChildrenMounted();
    void collapsingLargeDirectoryRemovesChildren();
    void expandingMountedDirectoryDoesNotReload();
    void watchesDirectoryAndRefreshesAfterExternalCreate();
    void idleTreeDoesNotPollRebuild();
    void refreshInsertsAndRemovesRowsWithoutReset();
    void createsItemsUnderPathAndFindsRowForPath();
    void renamesPathIncludingCaseChange();
    void deletesPathSafely();
    void sameNamedDeletesUseUniqueTrashPaths();
    void purgeTrashReclaimsLeftoverEntries();
};

// Stands in for the product's 250 ms watcher coalescing.
constexpr int kWatchCoalesceMs = 20;

static void waitForIdle(FileTreeModel &model)
{
    // qWaitFor rechecks every few milliseconds; QTRY_* sleeps in 50 ms steps,
    // which most loads finish well inside.
    QVERIFY(QTest::qWaitFor([&model]() { return !model.busy(); }, 5000));
}

// First visible row named `name`, or -1.
static int rowNamed(const FileTreeModel &model, const QString &name)
{
    for (int row = 0; row < model.rowCount(); ++row) {
        if (model.data(model.index(row, 0), FileTreeModel::NameRole).toString() == name) return row;
    }
    return -1;
}

static bool writeFile(const QString &path, const QByteArray &bytes = {})
{
    QFile file(path);
    return file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size();
}

// Contents of `path`, or a marker naming it when it cannot be read.
static QByteArray readFile(const QString &path)
{
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? file.readAll() : "<unreadable " + path.toUtf8() + '>';
}

// Empty files f00.txt, f01.txt, ... in `directory`.
static bool writeNumberedFiles(const QString &directory, int count)
{
    for (int i = 0; i < count; ++i) {
        if (!writeFile(QDir(directory).filePath(QStringLiteral("f%1.txt").arg(i, 2, 10, QLatin1Char('0')))))
            return false;
    }
    return true;
}

void FileTreeModelTest::initTestCase()
{
    QStandardPaths::setTestModeEnabled(true);
}

void FileTreeModelTest::exposesExpandedProjectRoot() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    QFile child(dir.filePath(QStringLiteral("alpha.txt")));
    QVERIFY(child.open(QIODevice::WriteOnly));
    child.close();

    FileTreeModel model;
    model.setRootPath(dir.path());
    waitForIdle(model);

    QCOMPARE(model.rowCount(), 2);
    const QModelIndex root = model.index(0, 0);
    QCOMPARE(model.data(root, FileTreeModel::NameRole).toString(), QFileInfo(dir.path()).fileName());
    QVERIFY(model.data(root, FileTreeModel::DirectoryRole).toBool());
    QVERIFY(model.data(root, FileTreeModel::ExpandedRole).toBool());
    QVERIFY(model.data(root, FileTreeModel::RootRole).toBool());
    QCOMPARE(model.data(root, FileTreeModel::DepthRole).toInt(), 0);

    const QModelIndex firstChild = model.index(1, 0);
    QVERIFY(!model.data(firstChild, FileTreeModel::RootRole).toBool());
    QCOMPARE(model.data(firstChild, FileTreeModel::DepthRole).toInt(), 1);

    // A file is no root; an empty path closes the tree.
    model.setRootPath(dir.filePath(QStringLiteral("alpha.txt")));
    QVERIFY(model.errorMessage().contains(QStringLiteral("not a directory")));
    QCOMPARE(model.rowCount(), 2);
    model.setRootPath(QString());
    QCOMPARE(model.rowCount(), 0);
    QVERIFY(model.rootPath().isEmpty());
}

void FileTreeModelTest::createsItemsInRootNode() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    FileTreeModel model;
    model.setRootPath(dir.path());
    waitForIdle(model);
    model.createFile(dir.path(), QStringLiteral("created.txt"));
    model.createFolder(QString(), QStringLiteral("created-folder"));

    QVERIFY(QFileInfo::exists(dir.filePath(QStringLiteral("created.txt"))));
    QVERIFY(QFileInfo(dir.filePath(QStringLiteral("created-folder"))).isDir());

    model.createFile(dir.path(), QStringLiteral("created.txt"));
    QVERIFY(model.errorMessage().contains(QStringLiteral("already exists")));
    model.clearError();
    QTemporaryDir outside;
    model.createFile(outside.path(), QStringLiteral("escaped.txt"));
    QVERIFY(model.errorMessage().contains(QStringLiteral("outside the project root")));
    QVERIFY(!QFileInfo::exists(QDir(outside.path()).filePath(QStringLiteral("escaped.txt"))));
}

void FileTreeModelTest::protectsRootFromRenameAndDelete() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString rootPath = QFileInfo(dir.path()).canonicalFilePath();

    FileTreeModel model;
    model.setRootPath(rootPath);
    waitForIdle(model);
    model.renamePath(rootPath, QStringLiteral("renamed-root"));
    model.deletePath(rootPath);

    QVERIFY(QFileInfo(rootPath).isDir());
    QCOMPARE(model.rootPath(), rootPath);
}

void FileTreeModelTest::rejectsInvalidNames() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    FileTreeModel model;
    model.setRootPath(dir.path());
    waitForIdle(model);
    QCOMPARE(model.rowCount(), 1); // root only

    model.createFile(dir.path(), QStringLiteral(".."));
    model.createFile(dir.path(), QStringLiteral("CON"));
    model.createFile(dir.path(), QStringLiteral("a:b.txt"));
    model.createFolder(dir.path(), QStringLiteral("."));

    // No children should have been added; invalid names never touch the FS.
    QCOMPARE(model.rowCount(), 1);
    QVERIFY(QDir(dir.path()).entryList(QDir::AllEntries | QDir::NoDotAndDotDot).isEmpty());
    QVERIFY(!model.errorMessage().isEmpty());
}

void FileTreeModelTest::preservesExpansionAfterCreate() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString nested = dir.filePath(QStringLiteral("nested"));
    QVERIFY(QDir().mkpath(nested));

    FileTreeModel model;
    model.setRootPath(dir.path());
    waitForIdle(model);

    int nestedRow = rowNamed(model, QStringLiteral("nested"));
    QVERIFY(nestedRow >= 0);
    model.toggleExpanded(nestedRow);
    waitForIdle(model);
    QVERIFY(model.data(model.index(nestedRow, 0), FileTreeModel::ExpandedRole).toBool());

    model.createFile(nested, QStringLiteral("child.txt"));
    waitForIdle(model);
    QVERIFY(QFileInfo::exists(QDir(nested).filePath(QStringLiteral("child.txt"))));

    nestedRow = rowNamed(model, QStringLiteral("nested"));
    QVERIFY(nestedRow >= 0);
    QVERIFY(model.data(model.index(nestedRow, 0), FileTreeModel::ExpandedRole).toBool());
}

void FileTreeModelTest::exposesGitAggregationAndCategories() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString testsDir = dir.filePath(QStringLiteral("tests"));
    const QString nestedDir = QDir(testsDir).filePath(QStringLiteral("unit"));
    const QString buildDir = dir.filePath(QStringLiteral("build-output"));
    QVERIFY(QDir().mkpath(testsDir));
    QVERIFY(QDir().mkpath(nestedDir));
    QVERIFY(QDir().mkpath(buildDir));
    QFile testFile(QDir(nestedDir).filePath(QStringLiteral("case.cpp")));
    QVERIFY(testFile.open(QIODevice::WriteOnly));
    testFile.close();
    QVERIFY(writeFile(dir.filePath(QStringLiteral("helper.dll"))));
    QVERIFY(writeFile(dir.filePath(QStringLiteral("parser_spec.js"))));

    FileTreeModel model;
    model.setRootPath(dir.path());
    waitForIdle(model);
    model.setGitStatusMap({{QStringLiteral("tests/unit/case.cpp"), QStringLiteral("modified")}}, dir.path());

    const int testsRow = rowNamed(model, QStringLiteral("tests"));
    const int buildRow = rowNamed(model, QStringLiteral("build-output"));
    QVERIFY(testsRow >= 0);
    QVERIFY(buildRow >= 0);
    QCOMPARE(model.data(model.index(testsRow, 0), FileTreeModel::AggregateGitStatusRole).toString(),
             QStringLiteral("modified"));
    QCOMPARE(model.data(model.index(testsRow, 0), FileTreeModel::CategoryRole).toString(), QStringLiteral("test"));
    QCOMPARE(model.data(model.index(buildRow, 0), FileTreeModel::CategoryRole).toString(), QStringLiteral("build"));
    // Loose files are classified by suffix and name.
    QCOMPARE(model.data(model.index(model.rowForPath(dir.filePath(QStringLiteral("helper.dll"))), 0),
                        FileTreeModel::CategoryRole).toString(), QStringLiteral("build"));
    QCOMPARE(model.data(model.index(model.rowForPath(dir.filePath(QStringLiteral("parser_spec.js"))), 0),
                        FileTreeModel::CategoryRole).toString(), QStringLiteral("test"));
}

void FileTreeModelTest::ignoredGitPathsDimWithoutPromotingOntoParents()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    QVERIFY(QDir().mkpath(dir.filePath(QStringLiteral("src"))));
    QVERIFY(QDir().mkpath(dir.filePath(QStringLiteral("build/obj"))));
    QFile tracked(dir.filePath(QStringLiteral("src/main.cpp")));
    QVERIFY(tracked.open(QIODevice::WriteOnly));
    tracked.close();
    QFile skipped(dir.filePath(QStringLiteral("src/skip.tmp")));
    QVERIFY(skipped.open(QIODevice::WriteOnly));
    skipped.close();
    QFile logFile(dir.filePath(QStringLiteral("notes.log")));
    QVERIFY(logFile.open(QIODevice::WriteOnly));
    logFile.close();

    FileTreeModel model;
    model.setRootPath(dir.path());
    waitForIdle(model);
    model.setGitStatusMap({
        {QStringLiteral("build"), QStringLiteral("ignored")},
        {QStringLiteral("notes.log"), QStringLiteral("ignored")},
        {QStringLiteral("src/main.cpp"), QStringLiteral("modified")},
        {QStringLiteral("src/skip.tmp"), QStringLiteral("ignored")},
    }, dir.path());

    int srcRow = rowNamed(model, QStringLiteral("src"));
    const int buildRow = rowNamed(model, QStringLiteral("build"));
    const int notesRow = rowNamed(model, QStringLiteral("notes.log"));
    QVERIFY(srcRow >= 0);
    QVERIFY(buildRow >= 0);
    QVERIFY(notesRow >= 0);
    QCOMPARE(model.data(model.index(buildRow, 0), FileTreeModel::GitStatusRole).toString(),
             QStringLiteral("ignored"));
    QCOMPARE(model.data(model.index(buildRow, 0), FileTreeModel::AggregateGitStatusRole).toString(),
             QStringLiteral("ignored"));
    QCOMPARE(model.data(model.index(notesRow, 0), FileTreeModel::GitStatusRole).toString(),
             QStringLiteral("ignored"));
    QCOMPARE(model.data(model.index(srcRow, 0), FileTreeModel::AggregateGitStatusRole).toString(),
             QStringLiteral("modified"));

    model.toggleExpanded(buildRow);
    waitForIdle(model);
    const int objRow = rowNamed(model, QStringLiteral("obj"));
    QVERIFY(objRow >= 0);
    QCOMPARE(model.data(model.index(objRow, 0), FileTreeModel::GitStatusRole).toString(),
             QStringLiteral("ignored"));
    QCOMPARE(model.data(model.index(objRow, 0), FileTreeModel::AggregateGitStatusRole).toString(),
             QStringLiteral("ignored"));

    srcRow = rowNamed(model, QStringLiteral("src"));
    QVERIFY(srcRow >= 0);
    model.toggleExpanded(srcRow);
    waitForIdle(model);
    const int mainRow = rowNamed(model, QStringLiteral("main.cpp"));
    const int skipRow = rowNamed(model, QStringLiteral("skip.tmp"));
    QVERIFY(mainRow >= 0);
    QVERIFY(skipRow >= 0);
    QCOMPARE(model.data(model.index(mainRow, 0), FileTreeModel::GitStatusRole).toString(),
             QStringLiteral("modified"));
    QCOMPARE(model.data(model.index(skipRow, 0), FileTreeModel::GitStatusRole).toString(),
             QStringLiteral("ignored"));
}

void FileTreeModelTest::buildsSafeCommandsForWindowsScripts_data()
{
    QTest::addColumn<QString>("name");
    QTest::addColumn<QString>("quotedName");
    QTest::newRow("bat") << QStringLiteral("build.bat") << QStringLiteral("build.bat");
    QTest::newRow("upper-case cmd") << QStringLiteral("deploy.CMD") << QStringLiteral("deploy.CMD");
    QTest::newRow("ps1 with quote") << QStringLiteral("test's script.ps1") << QStringLiteral("test''s script.ps1");
}

void FileTreeModelTest::buildsSafeCommandsForWindowsScripts()
{
    QFETCH(QString, name);
    QFETCH(QString, quotedName);
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    QVERIFY(writeFile(dir.filePath(name), "rem test\n"));

    FileTreeModel model;
    model.setRootPath(dir.path());
    waitForIdle(model);
    const int row = rowNamed(model, name);

    QVERIFY(model.canRunInProjectTerminal(row));
    const QString command = model.projectTerminalCommand(row);
    QVERIFY(command.startsWith(QStringLiteral("powershell.exe ")));
    QVERIFY(command.contains(QStringLiteral("-ExecutionPolicy Bypass")));
    QVERIFY(command.contains(QStringLiteral("-NoExit -Command")));
    const QString quotedPath = QDir::toNativeSeparators(model.rootPath() + QLatin1Char('/') + quotedName);
    QVERIFY2(command.contains(QStringLiteral("& '%1'").arg(quotedPath)), qPrintable(command));
    QCOMPARE(model.independentTerminalArguments(row),
             (QStringList{QStringLiteral("--terminal"), QStringLiteral("--cwd"), model.rootPath(),
                          QStringLiteral("--command"), command}));
}

void FileTreeModelTest::refusesToRunNonScriptsAndScriptNamedFolders()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    QVERIFY(writeFile(dir.filePath(QStringLiteral("notes.txt")), "rem test\n"));
    QVERIFY(QDir().mkpath(dir.filePath(QStringLiteral("fake.ps1"))));

    FileTreeModel model;
    model.setRootPath(dir.path());
    waitForIdle(model);
    const int notes = rowNamed(model, QStringLiteral("notes.txt"));
    const int folder = rowNamed(model, QStringLiteral("fake.ps1"));

    QVERIFY(notes >= 0);
    QVERIFY(!model.canRunInProjectTerminal(notes));
    QVERIFY(model.projectTerminalCommand(notes).isEmpty());
    QVERIFY(model.independentTerminalArguments(notes).isEmpty());
    QVERIFY(folder >= 0);
    QVERIFY(!model.canRunInProjectTerminal(folder));
    QVERIFY(!model.canRunInProjectTerminal(-1));
}

void FileTreeModelTest::createsSiblingsBesideSelectedFile() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString nested = dir.filePath(QStringLiteral("nested"));
    QVERIFY(QDir().mkpath(nested));
    QFile existing(QDir(nested).filePath(QStringLiteral("seed.txt")));
    QVERIFY(existing.open(QIODevice::WriteOnly));
    existing.close();

    FileTreeModel model;
    model.setRootPath(dir.path());
    waitForIdle(model);

    const int nestedRow = rowNamed(model, QStringLiteral("nested"));
    QVERIFY(nestedRow >= 0);
    model.toggleExpanded(nestedRow);
    waitForIdle(model);

    const int seedRow = rowNamed(model, QStringLiteral("seed.txt"));
    QVERIFY(seedRow >= 0);
    QVERIFY(!model.data(model.index(seedRow, 0), FileTreeModel::DirectoryRole).toBool());

    model.createFile(model.data(model.index(seedRow, 0), FileTreeModel::PathRole).toString(),
                     QStringLiteral("sibling.txt"));
    QVERIFY(QFileInfo::exists(QDir(nested).filePath(QStringLiteral("sibling.txt"))));
    QVERIFY(!QFileInfo::exists(dir.filePath(QStringLiteral("sibling.txt"))));
}

void FileTreeModelTest::renamesFileInsideProject()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    QFile seed(dir.filePath(QStringLiteral("old-name.txt")));
    QVERIFY(seed.open(QIODevice::WriteOnly));
    seed.write("keep\n");
    seed.close();

    FileTreeModel model;
    model.setRootPath(dir.path());
    waitForIdle(model);
    model.renamePath(dir.filePath(QStringLiteral("old-name.txt")), QStringLiteral("new-name.txt"));
    waitForIdle(model);
    QVERIFY(QFileInfo::exists(dir.filePath(QStringLiteral("new-name.txt"))));
    QVERIFY(!QFileInfo::exists(dir.filePath(QStringLiteral("old-name.txt"))));
    QCOMPARE(rowNamed(model, QStringLiteral("new-name.txt")) >= 0, true);
}

void FileTreeModelTest::rejectsRenameEscapeAndCollision()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    QFile first(dir.filePath(QStringLiteral("alpha.txt")));
    QVERIFY(first.open(QIODevice::WriteOnly));
    first.close();
    QFile second(dir.filePath(QStringLiteral("beta.txt")));
    QVERIFY(second.open(QIODevice::WriteOnly));
    second.close();

    FileTreeModel model;
    model.setRootPath(dir.path());
    waitForIdle(model);
    const QString alpha = dir.filePath(QStringLiteral("alpha.txt"));
    model.renamePath(alpha, QStringLiteral(".."));
    QVERIFY(QFileInfo::exists(dir.filePath(QStringLiteral("alpha.txt"))));
    QVERIFY(!model.errorMessage().isEmpty());

    model.clearError();
    model.renamePath(alpha, QStringLiteral("beta.txt"));
    QVERIFY(QFileInfo::exists(dir.filePath(QStringLiteral("alpha.txt"))));
    QVERIFY(model.errorMessage().contains(QStringLiteral("already exists")));
}

void FileTreeModelTest::deletesToInternalTrashAndUndoes()
{
    QTemporaryDir dir;
    QTemporaryDir trashDir;
    QVERIFY(dir.isValid());
    QVERIFY(trashDir.isValid());
    const QString path = dir.filePath(QStringLiteral("doomed.txt"));
    QFile file(path);
    QVERIFY(file.open(QIODevice::WriteOnly));
    file.write("recover me\n");
    file.close();

    FileTreeModel model;
    model.setTrashRootForTesting(trashDir.path());
    model.setRootPath(dir.path());
    waitForIdle(model);
    QVERIFY(!model.canUndo());
    model.deletePath(path);
    waitForIdle(model);
    QVERIFY(!QFileInfo::exists(path));
    QVERIFY(model.canUndo());
    QVERIFY(QFileInfo(trashDir.path()).isDir());
    const QStringList trash = QDir(trashDir.path()).entryList(
        QDir::Files | QDir::NoDotAndDotDot);
    QVERIFY(!trash.isEmpty());

    model.undoLastDelete();
    waitForIdle(model);
    QVERIFY(QFileInfo::exists(path));
    QVERIFY(!model.canUndo());
    QFile restored(path);
    QVERIFY(restored.open(QIODevice::ReadOnly));
    QCOMPARE(restored.readAll(), QByteArray("recover me\n"));
}

void FileTreeModelTest::rejectsNameEquivalenceClasses_data()
{
    QTest::addColumn<QString>("name");
    QTest::newRow("empty") << QString();
    QTest::newRow("whitespace") << QStringLiteral("   ");
    QTest::newRow("dot") << QStringLiteral(".");
    QTest::newRow("dotdot") << QStringLiteral("..");
    QTest::newRow("trailing-dot") << QStringLiteral("name.");
    QTest::newRow("trailing-space") << QStringLiteral("name ");
    QTest::newRow("reserved-CON") << QStringLiteral("CON");
    QTest::newRow("reserved-con-ext") << QStringLiteral("con.txt");
    QTest::newRow("colon") << QStringLiteral("a:b.txt");
    QTest::newRow("star") << QStringLiteral("a*.txt");
    QTest::newRow("pipe") << QStringLiteral("a|b.txt");
    QTest::newRow("slash") << QStringLiteral("a/b.txt");
    QTest::newRow("backslash") << QStringLiteral("a\\b.txt");
}

void FileTreeModelTest::rejectsNameEquivalenceClasses()
{
    QFETCH(QString, name);
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    FileTreeModel model;
    model.setRootPath(dir.path());
    waitForIdle(model);
    QCOMPARE(model.rowCount(), 1);
    model.createFile(dir.path(), name);
    QCOMPARE(model.rowCount(), 1);
    QVERIFY(QDir(dir.path()).entryList(QDir::AllEntries | QDir::NoDotAndDotDot).isEmpty());
    QVERIFY(!model.errorMessage().isEmpty());
}

void FileTreeModelTest::pathsOutsideRootDoNotMutateDisk()
{
    QTemporaryDir dir;
    QTemporaryDir outside;
    QVERIFY(dir.isValid());
    QVERIFY(outside.isValid());
    const QString foreign = outside.filePath(QStringLiteral("keep.txt"));
    QFile seed(foreign);
    QVERIFY(seed.open(QIODevice::WriteOnly));
    seed.write("keep\n");
    seed.close();

    FileTreeModel model;
    model.setRootPath(dir.path());
    waitForIdle(model);
    model.renamePath(foreign, QStringLiteral("x.txt"));
    model.deletePath(foreign);
    model.createFile(outside.path(), QStringLiteral("ghost.txt"));
    model.createFolder(foreign, QStringLiteral("ghost-dir"));
    QVERIFY(QFileInfo::exists(foreign));
    QVERIFY(!QFileInfo::exists(outside.filePath(QStringLiteral("x.txt"))));
    QVERIFY(!QFileInfo::exists(outside.filePath(QStringLiteral("ghost.txt"))));
    QVERIFY(!QFileInfo::exists(outside.filePath(QStringLiteral("ghost-dir"))));
    QVERIFY(!model.errorMessage().isEmpty());
}

void FileTreeModelTest::sameNamedDeletesUseUniqueTrashPaths()
{
    QTemporaryDir dir;
    QTemporaryDir trashDir;
    QVERIFY(dir.isValid());
    QVERIFY(trashDir.isValid());
    const QString firstDir = dir.filePath(QStringLiteral("first"));
    const QString secondDir = dir.filePath(QStringLiteral("second"));
    QVERIFY(QDir().mkpath(firstDir));
    QVERIFY(QDir().mkpath(secondDir));
    const QString first = QDir(firstDir).filePath(QStringLiteral("same.txt"));
    const QString second = QDir(secondDir).filePath(QStringLiteral("same.txt"));
    QVERIFY(writeFile(first, QByteArrayLiteral("one")));
    QVERIFY(writeFile(second, QByteArrayLiteral("two")));

    FileTreeModel model;
    model.setTrashRootForTesting(trashDir.path());
    model.setRootPath(dir.path());
    waitForIdle(model);
    model.deletePath(first);
    waitForIdle(model);
    model.deletePath(second);
    waitForIdle(model);

    QVERIFY(!QFileInfo::exists(first));
    QVERIFY(!QFileInfo::exists(second));
    const QStringList trashed = QDir(trashDir.path()).entryList(
        QDir::Files | QDir::NoDotAndDotDot);
    QCOMPARE(trashed.size(), 2);

    model.undoLastDelete();
    waitForIdle(model);
    model.undoLastDelete();
    waitForIdle(model);
    QFile firstRestored(first);
    QVERIFY(firstRestored.open(QIODevice::ReadOnly));
    QCOMPARE(firstRestored.readAll(), QByteArrayLiteral("one"));
    firstRestored.close();
    QFile secondRestored(second);
    QVERIFY(secondRestored.open(QIODevice::ReadOnly));
    QCOMPARE(secondRestored.readAll(), QByteArrayLiteral("two"));
}

void FileTreeModelTest::purgeTrashReclaimsLeftoverEntries()
{
    QTemporaryDir trashDir;
    QVERIFY(trashDir.isValid());
    const QString staleFile = QDir(trashDir.path())
        .filePath(QStringLiteral("20240101000000000_ab12_stale.txt"));
    QFile file(staleFile);
    QVERIFY(file.open(QIODevice::WriteOnly));
    file.write("leftover\n");
    file.close();
    QVERIFY(QDir().mkpath(QDir(trashDir.path()).filePath(QStringLiteral("sub"))));

    QVERIFY(FileTreeModel::purgeTrash(trashDir.path()));
    QVERIFY(!QFileInfo::exists(staleFile));
    QVERIFY(QDir(trashDir.path()).entryList(QDir::AllEntries | QDir::NoDotAndDotDot)
                .isEmpty()
            || !QFileInfo::exists(trashDir.path()));

    // Refuses to wipe an empty path or a filesystem root.
    QVERIFY(!FileTreeModel::purgeTrash(QString()));
}

void FileTreeModelTest::undoWithEmptyStackReportsError()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    FileTreeModel model;
    model.setRootPath(dir.path());
    waitForIdle(model);
    QVERIFY(!model.canUndo());
    model.undoLastDelete();
    QVERIFY(model.errorMessage().contains(QStringLiteral("Nothing to undo")));
    QVERIFY(!model.canUndo());
}

void FileTreeModelTest::deletesDirectoryTreeAndUndoes()
{
    QTemporaryDir dir;
    QTemporaryDir trashDir;
    QVERIFY(dir.isValid());
    QVERIFY(trashDir.isValid());
    const QString nested = dir.filePath(QStringLiteral("nested"));
    QVERIFY(QDir().mkpath(nested));
    QFile child(QDir(nested).filePath(QStringLiteral("child.txt")));
    QVERIFY(child.open(QIODevice::WriteOnly));
    child.write("nested-body\n");
    child.close();

    FileTreeModel model;
    model.setTrashRootForTesting(trashDir.path());
    model.setRootPath(dir.path());
    waitForIdle(model);
    model.deletePath(nested);
    waitForIdle(model);
    QVERIFY(!QFileInfo::exists(nested));
    QVERIFY(model.canUndo());
    model.undoLastDelete();
    waitForIdle(model);
    QVERIFY(QFileInfo::exists(QDir(nested).filePath(QStringLiteral("child.txt"))));
    QFile restored(QDir(nested).filePath(QStringLiteral("child.txt")));
    QVERIFY(restored.open(QIODevice::ReadOnly));
    QCOMPARE(restored.readAll(), QByteArray("nested-body\n"));
}

void FileTreeModelTest::renameKeepsDirectoryWhenItemInsideIsLocked()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString folder = dir.filePath(QStringLiteral("busy"));
    QVERIFY(QDir().mkpath(folder));
    const QString lockedPath = QDir(folder).filePath(QStringLiteral("locked.txt"));
    const QString freePath = QDir(folder).filePath(QStringLiteral("free.txt"));
    QVERIFY(writeFile(lockedPath, "keep\n"));
    QVERIFY(writeFile(freePath, "keep\n"));
    // No FILE_SHARE_DELETE: the folder cannot be renamed nor the file removed.
    const HANDLE lock = CreateFileW(reinterpret_cast<const wchar_t *>(lockedPath.utf16()),
                                    GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                                    FILE_ATTRIBUTE_NORMAL, nullptr);
    QVERIFY(lock != INVALID_HANDLE_VALUE);

    FileTreeModel model;
    model.setRootPath(dir.path());
    waitForIdle(model);
    model.renamePath(folder, QStringLiteral("renamed"));
    waitForIdle(model);
    CloseHandle(lock);

    QVERIFY(!model.errorMessage().isEmpty());
    QVERIFY(!QFileInfo::exists(dir.filePath(QStringLiteral("renamed"))));
    QCOMPARE(readFile(lockedPath), QByteArray("keep\n"));
    QCOMPARE(readFile(freePath), QByteArray("keep\n"));
}

void FileTreeModelTest::copyMoveDeletesTreeWithHiddenFilesAndUndoes()
{
    QTemporaryDir dir;
    QTemporaryDir trashDir;
    QVERIFY(dir.isValid());
    QVERIFY(trashDir.isValid());
    const QString folder = dir.filePath(QStringLiteral("moved"));
    const QString hidden = QDir(folder).filePath(QStringLiteral("deep/desktop.ini"));
    const QString empty = QDir(folder).filePath(QStringLiteral("empty"));
    QVERIFY(QDir().mkpath(QFileInfo(hidden).absolutePath()));
    QVERIFY(QDir().mkpath(empty));
    QFile file(hidden);
    QVERIFY(file.open(QIODevice::WriteOnly));
    file.write("hidden-body\n");
    file.close();
    QVERIFY(SetFileAttributesW(reinterpret_cast<const wchar_t *>(hidden.utf16()),
                               FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM));

    FileTreeModel model;
    model.setTrashRootForTesting(trashDir.path());
    model.setCopyMovesForTesting(true);
    model.setRootPath(dir.path());
    waitForIdle(model);
    QSignalSpy progress(&model, &FileTreeModel::progressChanged);
    model.deletePath(folder);
    waitForIdle(model);
    QCOMPARE(model.errorMessage(), QString());
    QVERIFY(!QFileInfo::exists(folder));
    QVERIFY(model.canUndo());
    const auto finished = std::find_if(progress.cbegin(), progress.cend(), [](const QList<QVariant> &args) {
        return args.at(0).toReal() == 1.0 && args.at(1).toString().contains(QStringLiteral("100%"));
    });
    QVERIFY(finished != progress.cend());

    // Undo copies back the same way; the hidden file and the empty folder
    // both survive the round trip.
    model.undoLastDelete();
    waitForIdle(model);
    QCOMPARE(model.errorMessage(), QString());
    QVERIFY(QFileInfo(empty).isDir());
    QFile restored(hidden);
    QVERIFY(restored.open(QIODevice::ReadOnly));
    QCOMPARE(restored.readAll(), QByteArray("hidden-body\n"));
    QVERIFY(QDir(trashDir.path())
                .entryList(QDir::AllEntries | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot)
                .isEmpty());
}

void FileTreeModelTest::copyMoveLeavesSourceWholeWhenItemInsideIsLocked()
{
    QTemporaryDir dir;
    QTemporaryDir trashDir;
    QVERIFY(dir.isValid());
    QVERIFY(trashDir.isValid());
    const QString folder = dir.filePath(QStringLiteral("busy"));
    QVERIFY(QDir().mkpath(folder));
    const QString lockedPath = QDir(folder).filePath(QStringLiteral("locked.txt"));
    const QString freePath = QDir(folder).filePath(QStringLiteral("free.txt"));
    QVERIFY(writeFile(lockedPath, "keep\n"));
    QVERIFY(writeFile(freePath, "keep\n"));
    // Readable, so the copy completes, but without FILE_SHARE_DELETE the
    // source cannot be removed afterwards.
    const HANDLE lock = CreateFileW(reinterpret_cast<const wchar_t *>(lockedPath.utf16()),
                                    GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                                    FILE_ATTRIBUTE_NORMAL, nullptr);
    QVERIFY(lock != INVALID_HANDLE_VALUE);

    FileTreeModel model;
    model.setTrashRootForTesting(trashDir.path());
    model.setCopyMovesForTesting(true);
    model.setRootPath(dir.path());
    waitForIdle(model);
    model.deletePath(folder);
    waitForIdle(model);
    CloseHandle(lock);

    QVERIFY2(model.errorMessage().contains(QStringLiteral("in use")),
             qPrintable(model.errorMessage()));
    QVERIFY(!model.canUndo());
    // Entries removed before the locked one are put back from the copy, and
    // the copy is dropped only once the source is whole again.
    QCOMPARE(readFile(lockedPath), QByteArray("keep\n"));
    QCOMPARE(readFile(freePath), QByteArray("keep\n"));
    QVERIFY(QDir(trashDir.path()).entryList(QDir::AllEntries | QDir::NoDotAndDotDot).isEmpty());
}

void FileTreeModelTest::undoFailsWhenOriginalFolderBecameAFile()
{
    QTemporaryDir dir;
    QTemporaryDir trashDir;
    QVERIFY(dir.isValid());
    QVERIFY(trashDir.isValid());
    const QString folder = dir.filePath(QStringLiteral("sub"));
    QVERIFY(QDir().mkpath(folder));
    QFile child(QDir(folder).filePath(QStringLiteral("a.txt")));
    QVERIFY(child.open(QIODevice::WriteOnly));
    child.close();

    FileTreeModel model;
    model.setTrashRootForTesting(trashDir.path());
    model.setRootPath(dir.path());
    waitForIdle(model);
    model.deletePath(child.fileName());
    waitForIdle(model);
    QVERIFY(model.canUndo());

    QVERIFY(QDir(folder).removeRecursively());
    QFile blocker(folder);
    QVERIFY(blocker.open(QIODevice::WriteOnly));
    blocker.close();
    model.undoLastDelete();
    waitForIdle(model);
    QVERIFY2(model.errorMessage().contains(QStringLiteral("Could not restore")),
             qPrintable(model.errorMessage()));
    // The entry stays restorable once the way is clear again.
    QVERIFY(model.canUndo());
    QCOMPARE(QDir(trashDir.path()).entryList(QDir::AllEntries | QDir::NoDotAndDotDot).size(), 1);
}

void FileTreeModelTest::acceptsUnicodeFileName()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    FileTreeModel model;
    model.setRootPath(dir.path());
    waitForIdle(model);
    model.createFile(dir.path(), QStringLiteral("中文.txt"));
    waitForIdle(model);
    QVERIFY(QFileInfo::exists(dir.filePath(QStringLiteral("中文.txt"))));
    QVERIFY(rowNamed(model, QStringLiteral("中文.txt")) >= 0);
}

void FileTreeModelTest::reportsBusyAroundRootLoad()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    QFile child(dir.filePath(QStringLiteral("alpha.txt")));
    QVERIFY(child.open(QIODevice::WriteOnly));
    child.close();

    FileTreeModel model;
    QSignalSpy busy(&model, &FileTreeModel::busyChanged);
    QVERIFY(!model.busy());
    model.setRootPath(dir.path());
    QVERIFY(model.busy());
    waitForIdle(model);
    QCOMPARE(busy.count(), 2);
    model.setRootPath(dir.path());
    QCOMPARE(busy.count(), 2);
}

void FileTreeModelTest::expandingDirectoryReportsRowLoadingWithoutClearingTree()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    QVERIFY(QDir(dir.path()).mkdir(QStringLiteral("nested")));
    QFile child(dir.filePath(QStringLiteral("nested/alpha.txt")));
    QVERIFY(child.open(QIODevice::WriteOnly));
    child.close();
    QFile sibling(dir.filePath(QStringLiteral("root.txt")));
    QVERIFY(sibling.open(QIODevice::WriteOnly));
    sibling.close();

    FileTreeModel model;
    model.setRootPath(dir.path());
    waitForIdle(model);
    QVERIFY(model.nodeCount() >= 2);

    const int nestedRow = rowNamed(model, QStringLiteral("nested"));
    QVERIFY(nestedRow >= 0);
    QVERIFY(!model.data(model.index(nestedRow), FileTreeModel::LoadingRole).toBool());
    model.toggleExpanded(nestedRow);
    QVERIFY(model.data(model.index(nestedRow), FileTreeModel::LoadingRole).toBool()
            || model.data(model.index(nestedRow), FileTreeModel::ExpandedRole).toBool());
    QVERIFY(model.nodeCount() >= 2);
    waitForIdle(model);
    QVERIFY(!model.data(model.index(nestedRow), FileTreeModel::LoadingRole).toBool());
}

void FileTreeModelTest::discardsStaleRootLoad()
{
    QTemporaryDir first;
    QTemporaryDir second;
    QVERIFY(first.isValid());
    QVERIFY(second.isValid());
    QFile firstFile(first.filePath(QStringLiteral("first.txt")));
    QVERIFY(firstFile.open(QIODevice::WriteOnly));
    firstFile.close();
    QFile secondFile(second.filePath(QStringLiteral("second.txt")));
    QVERIFY(secondFile.open(QIODevice::WriteOnly));
    secondFile.close();

    FileTreeModel model;
    model.setRootPath(first.path());
    model.setRootPath(second.path());
    waitForIdle(model);
    QCOMPARE(model.rootPath(), QFileInfo(second.path()).canonicalFilePath());
    QVERIFY(rowNamed(model, QStringLiteral("second.txt")) >= 0);
    QCOMPARE(rowNamed(model, QStringLiteral("first.txt")), -1);
}


void FileTreeModelTest::collapseDuringLoadDiscardsStaleChildren()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    QVERIFY(QDir().mkpath(dir.filePath(QStringLiteral("nested/deep"))));
    QFile child(dir.filePath(QStringLiteral("nested/alpha.txt")));
    QVERIFY(child.open(QIODevice::WriteOnly));
    child.close();

    FileTreeModel model;
    model.setRootPath(dir.path());
    waitForIdle(model);
    const int nestedRow = rowNamed(model, QStringLiteral("nested"));
    QVERIFY(nestedRow >= 0);
    const int rowsBefore = model.rowCount();

    model.toggleExpanded(nestedRow);
    model.toggleExpanded(nestedRow);
    QVERIFY(!model.data(model.index(nestedRow), FileTreeModel::ExpandedRole).toBool());

    QSignalSpy inserted(&model, &QAbstractItemModel::rowsInserted);
    waitForIdle(model);
    QCOMPARE(model.rowCount(), rowsBefore);
    QVERIFY(!model.data(model.index(nestedRow), FileTreeModel::ExpandedRole).toBool());
    QVERIFY(!model.data(model.index(nestedRow), FileTreeModel::LoadingRole).toBool());
    QCOMPARE(inserted.count(), 0);

    model.toggleExpanded(nestedRow);
    waitForIdle(model);
    QVERIFY(model.data(model.index(nestedRow), FileTreeModel::ExpandedRole).toBool());
    QVERIFY(rowNamed(model, QStringLiteral("alpha.txt")) >= 0);
}

void FileTreeModelTest::collapsingSmallDirectoryKeepsChildrenMounted()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    QVERIFY(QDir(dir.path()).mkdir(QStringLiteral("nested")));
    QFile first(dir.filePath(QStringLiteral("nested/alpha.txt")));
    QVERIFY(first.open(QIODevice::WriteOnly));
    first.close();
    QFile second(dir.filePath(QStringLiteral("nested/beta.txt")));
    QVERIFY(second.open(QIODevice::WriteOnly));
    second.close();

    FileTreeModel model;
    model.setRootPath(dir.path());
    waitForIdle(model);
    const int nestedRow = rowNamed(model, QStringLiteral("nested"));
    QVERIFY(nestedRow >= 0);
    model.toggleExpanded(nestedRow);
    waitForIdle(model);
    const int rows = model.rowCount();
    QVERIFY(rows >= 4);
    const int alpha = rowNamed(model, QStringLiteral("alpha.txt"));
    QVERIFY(alpha >= 0);
    QVERIFY(model.data(model.index(alpha), FileTreeModel::RevealedRole).toBool());

    model.toggleExpanded(nestedRow);
    QVERIFY(!model.data(model.index(nestedRow), FileTreeModel::ExpandedRole).toBool());
    QCOMPARE(model.rowCount(), rows);
    QVERIFY(!model.data(model.index(alpha), FileTreeModel::RevealedRole).toBool());

    model.toggleExpanded(nestedRow);
    QVERIFY(model.data(model.index(nestedRow), FileTreeModel::ExpandedRole).toBool());
    QCOMPARE(model.rowCount(), rows);
    QVERIFY(model.data(model.index(alpha), FileTreeModel::RevealedRole).toBool());
}

void FileTreeModelTest::collapsingLargeDirectoryRemovesChildren()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    QVERIFY(QDir(dir.path()).mkdir(QStringLiteral("nested")));
    const int extra = FileTreeModel::mountedChildLimit() + 1;
    QVERIFY(writeNumberedFiles(dir.filePath(QStringLiteral("nested")), extra));

    FileTreeModel model;
    model.setRootPath(dir.path());
    waitForIdle(model);
    const int nestedRow = rowNamed(model, QStringLiteral("nested"));
    QVERIFY(nestedRow >= 0);
    model.toggleExpanded(nestedRow);
    waitForIdle(model);
    QCOMPARE(model.rowCount(), nestedRow + 1 + extra);
    model.toggleExpanded(nestedRow);
    QVERIFY(!model.data(model.index(nestedRow), FileTreeModel::ExpandedRole).toBool());
    QCOMPARE(model.rowCount(), nestedRow + 1);
}

void FileTreeModelTest::expandingMountedDirectoryDoesNotReload()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    QVERIFY(QDir(dir.path()).mkdir(QStringLiteral("nested")));
    QFile child(dir.filePath(QStringLiteral("nested/alpha.txt")));
    QVERIFY(child.open(QIODevice::WriteOnly));
    child.close();

    FileTreeModel model;
    model.setRootPath(dir.path());
    waitForIdle(model);
    const int nestedRow = rowNamed(model, QStringLiteral("nested"));
    QVERIFY(nestedRow >= 0);
    model.toggleExpanded(nestedRow);
    waitForIdle(model);
    model.toggleExpanded(nestedRow);
    QVERIFY(!model.data(model.index(nestedRow), FileTreeModel::ExpandedRole).toBool());

    QSignalSpy busy(&model, &FileTreeModel::busyChanged);
    model.toggleExpanded(nestedRow);
    QVERIFY(model.data(model.index(nestedRow), FileTreeModel::ExpandedRole).toBool());
    QCOMPARE(busy.count(), 0);
    QVERIFY(!model.busy());
    QVERIFY(rowNamed(model, QStringLiteral("alpha.txt")) >= 0);
}

void FileTreeModelTest::watchesDirectoryAndRefreshesAfterExternalCreate()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    QFile first(dir.filePath(QStringLiteral("alpha.txt")));
    QVERIFY(first.open(QIODevice::WriteOnly));
    first.close();

    FileTreeModel model;
    model.setWatchCoalesceIntervalForTesting(kWatchCoalesceMs);
    model.setRootPath(dir.path());
    waitForIdle(model);
    QTest::qWait(3 * kWatchCoalesceMs);
    QVERIFY(rowNamed(model, QStringLiteral("alpha.txt")) >= 0);
    QSignalSpy busy(&model, &FileTreeModel::busyChanged);

    QFile second(dir.filePath(QStringLiteral("beta.txt")));
    QVERIFY(second.open(QIODevice::WriteOnly));
    second.close();
    QTRY_VERIFY_WITH_TIMEOUT(rowNamed(model, QStringLiteral("beta.txt")) >= 0, 5000);
    QCOMPARE(busy.count(), 0);
    QVERIFY(!model.busy());
}

void FileTreeModelTest::idleTreeDoesNotPollRebuild()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    QFile child(dir.filePath(QStringLiteral("alpha.txt")));
    QVERIFY(child.open(QIODevice::WriteOnly));
    child.close();

    FileTreeModel model;
    model.setWatchCoalesceIntervalForTesting(kWatchCoalesceMs);
    model.setRootPath(dir.path());
    waitForIdle(model);
    QTest::qWait(3 * kWatchCoalesceMs);
    QSignalSpy reset(&model, &QAbstractItemModel::modelReset);
    QTest::qWait(500);
    QCOMPARE(reset.count(), 0);
    QVERIFY(!model.busy());
}

static QStringList rowNames(const FileTreeModel &model)
{
    QStringList names;
    for (int row = 0; row < model.rowCount(); ++row)
        names.push_back(model.data(model.index(row, 0), FileTreeModel::NameRole).toString());
    return names;
}

static void touch(const QString &path)
{
    QFile file(path);
    QVERIFY(file.open(QIODevice::WriteOnly));
    file.close();
}

void FileTreeModelTest::refreshInsertsAndRemovesRowsWithoutReset()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    QVERIFY(QDir(dir.path()).mkdir(QStringLiteral("nested")));
    touch(dir.filePath(QStringLiteral("nested/one.txt")));
    touch(dir.filePath(QStringLiteral("nested/two.txt")));
    touch(dir.filePath(QStringLiteral("nested/zeta.txt")));
    touch(dir.filePath(QStringLiteral("alpha.txt")));

    FileTreeModel model;
    model.setRootPath(dir.path());
    waitForIdle(model);
    model.toggleExpanded(rowNamed(model, QStringLiteral("nested")));
    waitForIdle(model);
    const QString rootName = QFileInfo(dir.path()).fileName();
    QCOMPARE(rowNames(model),
             (QStringList{rootName, QStringLiteral("nested"), QStringLiteral("one.txt"), QStringLiteral("two.txt"),
                          QStringLiteral("zeta.txt"), QStringLiteral("alpha.txt")}));
    const QPersistentModelIndex alpha = model.index(rowNamed(model, QStringLiteral("alpha.txt")));

    QSignalSpy reset(&model, &QAbstractItemModel::modelReset);
    QSignalSpy inserted(&model, &QAbstractItemModel::rowsInserted);
    QSignalSpy removed(&model, &QAbstractItemModel::rowsRemoved);
    touch(dir.filePath(QStringLiteral("nested/three.txt")));
    QVERIFY(QFile::remove(dir.filePath(QStringLiteral("nested/zeta.txt"))));
    model.refresh();
    waitForIdle(model);

    QCOMPARE(rowNames(model),
             (QStringList{rootName, QStringLiteral("nested"), QStringLiteral("one.txt"), QStringLiteral("three.txt"),
                          QStringLiteral("two.txt"), QStringLiteral("alpha.txt")}));
    QCOMPARE(reset.count(), 0);
    QCOMPARE(inserted.count(), 1);
    QCOMPARE(inserted.at(0).at(1).toInt(), 3);
    QCOMPARE(inserted.at(0).at(2).toInt(), 3);
    QCOMPARE(removed.count(), 1);
    QCOMPARE(removed.at(0).at(1).toInt(), 5);
    QCOMPARE(removed.at(0).at(2).toInt(), 5);
    // Rows outside the change keep their identity, so views keep delegates.
    QVERIFY(alpha.isValid());
    QCOMPARE(model.data(alpha, FileTreeModel::NameRole).toString(), QStringLiteral("alpha.txt"));

    // Removing the expanded directory drops its whole subtree in one run.
    inserted.clear();
    removed.clear();
    QVERIFY(QDir(dir.filePath(QStringLiteral("nested"))).removeRecursively());
    model.refresh();
    waitForIdle(model);
    QCOMPARE(rowNames(model), (QStringList{rootName, QStringLiteral("alpha.txt")}));
    QCOMPARE(reset.count(), 0);
    QCOMPARE(inserted.count(), 0);
    QCOMPARE(removed.count(), 1);
    QCOMPARE(removed.at(0).at(1).toInt(), 1);
    QCOMPARE(removed.at(0).at(2).toInt(), 4);
}

void FileTreeModelTest::createsItemsUnderPathAndFindsRowForPath()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    FileTreeModel model;
    model.setRootPath(dir.path());
    waitForIdle(model);

    model.createFile(dir.path(), QStringLiteral("hello.txt"));
    waitForIdle(model);
    QVERIFY(QFileInfo::exists(dir.filePath(QStringLiteral("hello.txt"))));

    model.createFolder(dir.path(), QStringLiteral("nested_dir"));
    waitForIdle(model);
    QVERIFY(QFileInfo(dir.filePath(QStringLiteral("nested_dir"))).isDir());

    const int rootRow = model.rowForPath(dir.path());
    QCOMPARE(rootRow, 0);

    const int helloRow = model.rowForPath(dir.filePath(QStringLiteral("hello.txt")));
    QVERIFY(helloRow > 0);

    const int nestedRow = model.rowForPath(dir.filePath(QStringLiteral("nested_dir")));
    QVERIFY(nestedRow > 0);

    // Create a file under the nested folder path directly
    model.createFile(dir.filePath(QStringLiteral("nested_dir")), QStringLiteral("inside.txt"));
    waitForIdle(model);
    QVERIFY(QFileInfo::exists(dir.filePath(QStringLiteral("nested_dir/inside.txt"))));
}

void FileTreeModelTest::renamesPathIncludingCaseChange()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString oldPath = dir.filePath(QStringLiteral("case_test.txt"));
    QFile file(oldPath);
    QVERIFY(file.open(QIODevice::WriteOnly));
    file.write("sample");
    file.close();

    FileTreeModel model;
    model.setRootPath(dir.path());
    waitForIdle(model);

    // Case-only rename
    model.renamePath(oldPath, QStringLiteral("CASE_TEST.TXT"));
    waitForIdle(model);
    QVERIFY(model.errorMessage().isEmpty());
    QVERIFY(QFileInfo::exists(dir.filePath(QStringLiteral("CASE_TEST.TXT"))));

    // Normal rename
    model.renamePath(dir.filePath(QStringLiteral("CASE_TEST.TXT")), QStringLiteral("renamed.txt"));
    waitForIdle(model);
    QVERIFY(model.errorMessage().isEmpty());
    QVERIFY(QFileInfo::exists(dir.filePath(QStringLiteral("renamed.txt"))));
}

void FileTreeModelTest::deletesPathSafely()
{
    QTemporaryDir dir;
    QTemporaryDir trashDir;
    QVERIFY(dir.isValid());
    QVERIFY(trashDir.isValid());
    const QString doomedPath = dir.filePath(QStringLiteral("path_to_delete.txt"));
    QFile file(doomedPath);
    QVERIFY(file.open(QIODevice::WriteOnly));
    file.write("delete me");
    file.close();

    FileTreeModel model;
    model.setTrashRootForTesting(trashDir.path());
    model.setRootPath(dir.path());
    waitForIdle(model);

    model.deletePath(doomedPath);
    waitForIdle(model);
    QVERIFY(!QFileInfo::exists(doomedPath));
    QVERIFY(model.canUndo());

    model.undoLastDelete();
    waitForIdle(model);
    QVERIFY(QFileInfo::exists(doomedPath));
}

QTEST_MAIN(FileTreeModelTest)
#include "file_tree_model_test.moc"
