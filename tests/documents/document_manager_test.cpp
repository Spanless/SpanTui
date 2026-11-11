#include "documents/document_manager.h"
#include "core/workspace_database_migration.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSignalSpy>
#include <QSqlDatabase>
#include <QTemporaryDir>
#include <QTest>

class DocumentManagerTest final : public QObject {
    Q_OBJECT
private slots:
    void opensOnceAndRestoresTabsFromDisk();
    void isolatesTabsByProjectAndEditorTile();
    void persistsTabOrderActiveTabAndClosedTabs();
    void marksDeletedOpenFileMissing();
    void reloadsCleanExternalChange();
    void migratesRenamedAndDeletedPaths();
    void rejectsBinaryAndOversizedFiles();
    void acceptsFileAtExactSizeLimit();
    void reloadsMultipleExternalChangesIndependently();
    void largeOpenReportsLoadingAndSmallOpenDoesNot();
    void largeOpenDoesNotMarkOtherDocumentLoading();
    void largeOpenAnnouncesActiveDocumentWhenTextArrives();
    void exactOneMiBExternalRefreshIsAsync();
    void refreshRestoresMissingFileWhenRecreated();
    void largeRestoreReadsAsynchronously();
};

static QStringList tabNames(const DocumentManager &manager)
{
    QStringList result;
    for (int i = 0; i < manager.rowCount(); ++i) result << QFileInfo(manager.pathAt(i)).fileName();
    return result;
}

// DocumentManager reads the schema owned by WorkspaceDatabaseMigration, which
// the workspace runs before any document connection opens.
static QString migratedDatabase(const QString &path) {
    const QString connection = QStringLiteral("document_manager_test_migration");
    {
        QSqlDatabase database = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), connection);
        database.setDatabaseName(path);
        QString error;
        if (!database.open() || !span::core::migrateWorkspaceDatabase(database, &error)) {
            qFatal("Cannot migrate test database %s: %s", qPrintable(path), qPrintable(error));
        }
    }
    QSqlDatabase::removeDatabase(connection);
    return path;
}

static void writeBytes(const QString &path, const QByteArray &bytes) {
    QFile file(path);
    QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
    QCOMPARE(file.write(bytes), bytes.size());
}

// An external edit made after opening lands on a later modification time.
// Qt's Windows watcher reports a file only when its mtime changes (size is
// not compared), and NTFS stamps writes from a clock that ticks every
// 15.6 ms unless something raised the timer resolution, as on CI; a rewrite
// right after opening would keep the mtime the watcher recorded.
static void writeExternalChange(const QString &path, const QByteArray &bytes) {
    const QDateTime opened = QFileInfo(path).lastModified();
    writeBytes(path, bytes);
    QFile file(path);
    QVERIFY(file.open(QIODevice::ReadWrite));
    QVERIFY(file.setFileTime(opened.addSecs(1), QFileDevice::FileModificationTime));
}

void DocumentManagerTest::opensOnceAndRestoresTabsFromDisk() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString filePath = dir.filePath("notes.md");
    const QString dbPath = dir.filePath("state.db");
    writeBytes(filePath, "# disk\n");
    {
        DocumentManager manager;
        QVERIFY(manager.initialize(migratedDatabase(dbPath)));
        QVERIFY(manager.openFile(filePath));
        QVERIFY(manager.openFile(filePath));
        QCOMPARE(manager.rowCount(), 1);
        manager.setDocumentCursor(manager.activeIndex(), 7);
        QVERIFY(manager.activeMarkdown());
        manager.flush();
    }
    {
        DocumentManager restored;
        QVERIFY(restored.initialize(migratedDatabase(dbPath)));
        QCOMPARE(restored.rowCount(), 1);
        QCOMPARE(restored.activeText(), QString("# disk\n"));
        QVERIFY(restored.activeMarkdown());
        QCOMPARE(restored.cursorAt(restored.activeIndex()), 7);
    }
    // Content is never stored: a tab whose file vanished restores empty.
    QVERIFY(QFile::remove(filePath));
    {
        DocumentManager restored;
        QVERIFY(restored.initialize(migratedDatabase(dbPath)));
        QCOMPARE(restored.rowCount(), 1);
        QVERIFY(restored.activeMissing());
        QVERIFY(restored.activeText().isEmpty());
    }
}

void DocumentManagerTest::isolatesTabsByProjectAndEditorTile()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString filePath = dir.filePath(QStringLiteral("shared.txt"));
    const QString dbPath = dir.filePath(QStringLiteral("state.db"));
    writeBytes(filePath, "disk");
    {
        DocumentManager first;
        first.setOwnerScope(dir.path(), QStringLiteral("editor-a"));
        QVERIFY(first.initialize(migratedDatabase(dbPath)));
        QVERIFY(first.openFile(filePath));
        first.setDocumentCursor(first.activeIndex(), 1);
        first.flush();

        DocumentManager second;
        second.setOwnerScope(dir.path(), QStringLiteral("editor-b"));
        QVERIFY(second.initialize(migratedDatabase(dbPath)));
        QVERIFY(second.openFile(filePath));
        second.setDocumentCursor(second.activeIndex(), 2);
        second.flush();
    }
    {
        DocumentManager first;
        first.setOwnerScope(dir.path(), QStringLiteral("editor-a"));
        QVERIFY(first.initialize(migratedDatabase(dbPath)));
        QCOMPARE(first.rowCount(), 1);
        QCOMPARE(first.activeText(), QStringLiteral("disk"));
        QCOMPARE(first.cursorAt(first.activeIndex()), 1);

        DocumentManager second;
        second.setOwnerScope(dir.path(), QStringLiteral("editor-b"));
        QVERIFY(second.initialize(migratedDatabase(dbPath)));
        QCOMPARE(second.rowCount(), 1);
        QCOMPARE(second.activeText(), QStringLiteral("disk"));
        QCOMPARE(second.cursorAt(second.activeIndex()), 2);
    }
}

void DocumentManagerTest::persistsTabOrderActiveTabAndClosedTabs()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString dbPath = migratedDatabase(dir.filePath(QStringLiteral("state.db")));
    const QStringList names{QStringLiteral("a.txt"), QStringLiteral("b.txt"),
                            QStringLiteral("c.txt")};
    writeBytes(dir.filePath(QStringLiteral("a.txt")), "a.txt");
    writeBytes(dir.filePath(QStringLiteral("b.txt")), "b.txt");
    writeBytes(dir.filePath(QStringLiteral("c.txt")), "c.txt");
    {
        DocumentManager manager;
        QVERIFY(manager.initialize(dbPath));
        QVERIFY(manager.openFile(dir.filePath(QStringLiteral("a.txt"))));
        QVERIFY(manager.openFile(dir.filePath(QStringLiteral("b.txt"))));
        QVERIFY(manager.openFile(dir.filePath(QStringLiteral("c.txt"))));
        manager.activate(1);
        manager.flush();
    }
    {
        DocumentManager restored;
        QVERIFY(restored.initialize(dbPath));
        QCOMPARE(tabNames(restored), names);
        QCOMPARE(restored.activeIndex(), 1);
        QCOMPARE(restored.activeText(), QStringLiteral("b.txt"));
        // Closing a tab removes its row; the rest keep their order.
        QVERIFY(restored.closeDocument(0));
        restored.flush();
    }
    {
        DocumentManager restored;
        QVERIFY(restored.initialize(dbPath));
        QCOMPARE(tabNames(restored), (QStringList{names.at(1), names.at(2)}));
        QCOMPARE(restored.activeIndex(), 0);
        QVERIFY(restored.closeDocument(1));
        QVERIFY(restored.closeDocument(0));
        restored.flush();
    }
    DocumentManager empty;
    QVERIFY(empty.initialize(dbPath));
    QCOMPARE(empty.rowCount(), 0);
    QCOMPARE(empty.activeIndex(), -1);
}

void DocumentManagerTest::marksDeletedOpenFileMissing() {
    QTemporaryDir dir;
    const QString filePath = dir.filePath("gone.txt");
    writeBytes(filePath, "one");
    DocumentManager manager;
    QVERIFY(manager.initialize(migratedDatabase(dir.filePath("state.db"))));
    QVERIFY(manager.openFile(filePath));
    QVERIFY(QFile::remove(filePath));
    manager.markPathDeleted(filePath);
    QVERIFY(manager.activeMissing());
    QCOMPARE(manager.activeText(), QString("one"));
}

void DocumentManagerTest::reloadsCleanExternalChange() {
    QTemporaryDir dir;
    const QString filePath = dir.filePath("clean.txt");
    writeBytes(filePath, "one");
    DocumentManager manager;
    QVERIFY(manager.initialize(migratedDatabase(dir.filePath("state.db"))));
    QVERIFY(manager.openFile(filePath));
    writeExternalChange(filePath, "two");
    QTRY_COMPARE_WITH_TIMEOUT(manager.activeText(), QString("two"), 3000);
}

void DocumentManagerTest::migratesRenamedAndDeletedPaths() {
    QTemporaryDir dir;
    const QString folder = dir.filePath("old");
    const QString renamedFolder = dir.filePath("new");
    QVERIFY(QDir().mkpath(folder));
    const QString original = QDir(folder).filePath("nested.txt");
    const QString renamed = QDir(renamedFolder).filePath("nested.txt");
    writeBytes(original, "body");
    DocumentManager manager;
    QVERIFY(manager.initialize(migratedDatabase(dir.filePath("state.db"))));
    QVERIFY(manager.openFile(original));
    QVERIFY(QDir().rename(folder, renamedFolder));
    manager.updatePath(folder, renamedFolder);
    QCOMPARE(manager.pathAt(manager.activeIndex()), QFileInfo(renamed).canonicalFilePath());
    QVERIFY(QFile::remove(renamed));
    manager.markPathDeleted(renamed);
    QVERIFY(manager.activeMissing());
    QCOMPARE(manager.activeText(), QString("body"));
    writeBytes(renamed, "body");
    manager.refreshPath(renamed);
    QVERIFY(!manager.activeMissing());
}

void DocumentManagerTest::rejectsBinaryAndOversizedFiles() {
    QTemporaryDir dir;
    const QString binary = dir.filePath("binary.bin");
    writeBytes(binary, QByteArray("a\0b", 3));
    const QString large = dir.filePath("large.txt");
    writeBytes(large, QByteArray(8 * 1024 * 1024 + 1, 'x'));
    DocumentManager manager;
    QVERIFY(manager.initialize(migratedDatabase(dir.filePath("state.db"))));
    QSignalSpy errors(&manager, &DocumentManager::errorOccurred);
    QVERIFY(!manager.openFile(binary));
    QVERIFY(manager.openFile(large));
    QTRY_COMPARE_WITH_TIMEOUT(errors.size(), 2, 5000);
    QCOMPARE(manager.rowCount(), 0);
}

void DocumentManagerTest::acceptsFileAtExactSizeLimit()
{
    QTemporaryDir dir;
    const QString path = dir.filePath("limit.txt");
    writeBytes(path, QByteArray(8 * 1024 * 1024, 'x'));
    DocumentManager manager;
    QVERIFY(manager.initialize(migratedDatabase(dir.filePath("state.db"))));
    QSignalSpy finished(&manager, &DocumentManager::progressFinished);
    QVERIFY(manager.openFile(path));
    QTRY_COMPARE_WITH_TIMEOUT(finished.count(), 1, 5000);
    QCOMPARE(manager.rowCount(), 1);
    QCOMPARE(manager.activeText().toUtf8().size(), 8 * 1024 * 1024);
}

void DocumentManagerTest::reloadsMultipleExternalChangesIndependently() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString first = dir.filePath("a.txt");
    const QString second = dir.filePath("b.txt");
    writeBytes(first, "a0\n");
    writeBytes(second, "b0\n");
    DocumentManager manager;
    QVERIFY(manager.initialize(migratedDatabase(dir.filePath("state.db"))));
    QVERIFY(manager.openFile(first));
    QVERIFY(manager.openFile(second));
    QCOMPARE(manager.rowCount(), 2);

    writeExternalChange(first, "a1\n");
    writeExternalChange(second, "b1\n");
    QTRY_COMPARE_WITH_TIMEOUT(manager.textAt(0), QString("a1\n"), 2000);
    QTRY_COMPARE_WITH_TIMEOUT(manager.textAt(1), QString("b1\n"), 2000);
}

void DocumentManagerTest::largeOpenReportsLoadingAndSmallOpenDoesNot()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    DocumentManager manager;
    QVERIFY(manager.initialize(migratedDatabase(dir.filePath(QStringLiteral("docs.db")))));

    const QString smallPath = dir.filePath(QStringLiteral("small.txt"));
    writeBytes(smallPath, QByteArray("tiny\n"));
    QSignalSpy started(&manager, &DocumentManager::progressStarted);
    QSignalSpy finished(&manager, &DocumentManager::progressFinished);
    QVERIFY(manager.openFile(smallPath));
    QCOMPARE(started.count(), 0);
    QCOMPARE(finished.count(), 0);

    const QString largePath = dir.filePath(QStringLiteral("large.txt"));
    writeBytes(largePath, QByteArray(1024 * 1024 + 8, 'a'));
    QVERIFY(manager.openFile(largePath));
    QCOMPARE(started.count(), 1);
    QTRY_COMPARE_WITH_TIMEOUT(finished.count(), 1, 5000);
    // The progress title names the file being opened.
    QVERIFY(started.at(0).at(0).toString().contains(QStringLiteral("large.txt")));
}

void DocumentManagerTest::largeOpenDoesNotMarkOtherDocumentLoading()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    DocumentManager manager;
    QVERIFY(manager.initialize(migratedDatabase(dir.filePath(QStringLiteral("docs.db")))));

    const QString smallPath = dir.filePath(QStringLiteral("small.txt"));
    writeBytes(smallPath, QByteArray("tiny\n"));
    QVERIFY(manager.openFile(smallPath));
    QCOMPARE(manager.activeIndex(), 0);
    QVERIFY(!manager.loadingAt(0));

    const QString largePath = dir.filePath(QStringLiteral("large.txt"));
    writeBytes(largePath, QByteArray(1024 * 1024 + 8, 'a'));
    QVERIFY(manager.openFile(largePath));
    QVERIFY(manager.loading());
    QVERIFY(manager.loadingAt(manager.activeIndex()));
    const int smallIndex = int(tabNames(manager).indexOf(QStringLiteral("small.txt")));
    QVERIFY(smallIndex >= 0);
    QVERIFY(!manager.loadingAt(smallIndex));
    QTRY_VERIFY_WITH_TIMEOUT(!manager.loading(), 5000);
    QVERIFY(!manager.loadingAt(manager.activeIndex()));
}

void DocumentManagerTest::largeOpenAnnouncesActiveDocumentWhenTextArrives()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    DocumentManager manager;
    QVERIFY(manager.initialize(migratedDatabase(dir.filePath(QStringLiteral("docs.db")))));

    const qsizetype largeSize = 1024 * 1024 + 8;
    const QString largePath = dir.filePath(QStringLiteral("large.txt"));
    writeBytes(largePath, QByteArray(largeSize, 'a'));
    QVERIFY(manager.openFile(largePath));
    QVERIFY(manager.loading());

    // The code preview rebuilds only on activeDocumentChanged, so the text
    // must already be there when that signal follows the asynchronous read.
    qsizetype announcedSize = -1;
    connect(&manager, &DocumentManager::activeDocumentChanged, &manager,
            [&] { announcedSize = manager.activeText().size(); });
    QTRY_VERIFY_WITH_TIMEOUT(!manager.loading(), 5000);
    QCOMPARE(announcedSize, largeSize);
}

void DocumentManagerTest::exactOneMiBExternalRefreshIsAsync()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("large.txt"));
    writeBytes(path, QByteArray(1024 * 1024, 'a'));
    DocumentManager manager;
    QVERIFY(manager.initialize(migratedDatabase(dir.filePath(QStringLiteral("docs.db")))));
    QSignalSpy loadingFinished(&manager, &DocumentManager::progressFinished);
    QVERIFY(manager.openFile(path));
    QTRY_COMPARE_WITH_TIMEOUT(loadingFinished.count(), 1, 5000);

    QByteArray external(1024 * 1024, 'c');
    writeBytes(path, external);
    const int priorFinished = loadingFinished.count();
    manager.refreshPath(path);
    QTRY_VERIFY_WITH_TIMEOUT(loadingFinished.count() > priorFinished, 5000);
    QTRY_COMPARE_WITH_TIMEOUT(manager.activeText().at(0), QChar('c'), 5000);
}

void DocumentManagerTest::refreshRestoresMissingFileWhenRecreated()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("vanish.txt"));
    const QString dbPath = dir.filePath(QStringLiteral("docs.db"));
    writeBytes(path, QByteArray("original\n"));

    DocumentManager manager;
    QVERIFY(manager.initialize(migratedDatabase(dbPath)));
    QVERIFY(manager.openFile(path));
    QCOMPARE(manager.activeText(), QString("original\n"));
    QVERIFY(!manager.activeMissing());

    // Delete file and mark deleted
    QVERIFY(QFile::remove(path));
    manager.markPathDeleted(path);
    QVERIFY(manager.activeMissing());

    // Refresh while still missing: should remain missing
    manager.refreshPath(path);
    QVERIFY(manager.activeMissing());

    // Recreate file
    writeBytes(path, QByteArray("resurrected\n"));

    // Refresh again: should clear missing and update content
    manager.refreshPath(path);
    QVERIFY(!manager.activeMissing());
    QCOMPARE(manager.activeText(), QString("resurrected\n"));

    // Persist via upsert and verify clean restoration
    manager.setDocumentCursor(manager.activeIndex(), 3);
    manager.flush();

    DocumentManager restored;
    QVERIFY(restored.initialize(migratedDatabase(dbPath)));
    QCOMPARE(restored.rowCount(), 1);
    QCOMPARE(restored.activeText(), QString("resurrected\n"));
    QVERIFY(!restored.activeMissing());
    QCOMPARE(restored.cursorAt(restored.activeIndex()), 3);
}

void DocumentManagerTest::largeRestoreReadsAsynchronously()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString dbPath = migratedDatabase(dir.filePath(QStringLiteral("docs.db")));
    const QString smallPath = dir.filePath(QStringLiteral("small.txt"));
    writeBytes(smallPath, QByteArray("tiny\n"));
    const QString largePath = dir.filePath(QStringLiteral("large.txt"));
    const qsizetype largeSize = 1024 * 1024 + 8;
    writeBytes(largePath, QByteArray(largeSize, 'b'));
    {
        DocumentManager manager;
        QVERIFY(manager.initialize(dbPath));
        QVERIFY(manager.openFile(smallPath));
        QVERIFY(manager.openFile(largePath));
        QTRY_VERIFY_WITH_TIMEOUT(!manager.loading(), 5000);
        manager.flush();
    }

    DocumentManager restored;
    QSignalSpy started(&restored, &DocumentManager::progressStarted);
    QSignalSpy activeText(&restored, &DocumentManager::activeTextChanged);
    QVERIFY(restored.initialize(dbPath));
    // Small tabs are read in place; the large one loads like a large open.
    QCOMPARE(restored.rowCount(), 2);
    QCOMPARE(restored.textAt(0), QStringLiteral("tiny\n"));
    QCOMPARE(restored.activeIndex(), 1);
    QVERIFY(restored.loadingAt(1));
    QVERIFY(restored.textAt(1).isEmpty());
    QCOMPARE(started.count(), 1);
    QVERIFY(started.at(0).at(0).toString().contains(QStringLiteral("large.txt")));
    const int announced = activeText.count();

    QTRY_VERIFY_WITH_TIMEOUT(!restored.loading(), 5000);
    QVERIFY(!restored.loadingAt(1));
    QCOMPARE(restored.textAt(1).size(), largeSize);
    QVERIFY(!restored.activeMissing());
    // The active tab announces its text once it arrives.
    QVERIFY(activeText.count() > announced);
}

QTEST_MAIN(DocumentManagerTest)
#include "document_manager_test.moc"
