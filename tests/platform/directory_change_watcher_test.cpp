#include "platform/windows/directory_change_watcher.h"

#include <QDir>
#include <QFile>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>

using span::platform::DirectoryChangeWatcher;

class DirectoryChangeWatcherTest final : public QObject {
    Q_OBJECT

private slots:
    void reportsChangesAnywhereInTheTree();
    void stopsAndRefusesMissingDirectory();
};

namespace {

bool writeFile(const QString &path, const QByteArray &contents)
{
    QFile file(path);
    return file.open(QIODevice::WriteOnly | QIODevice::Truncate) && file.write(contents) == contents.size();
}

// Every path reported so far; an empty batch (an overflow) is kept as "*".
QStringList reportedPaths(const QSignalSpy &spy)
{
    QStringList paths;
    for (const QList<QVariant> &arguments : spy) {
        const QStringList batch = arguments.at(0).toStringList();
        if (batch.isEmpty()) paths.append(QStringLiteral("*"));
        paths.append(batch);
    }
    return paths;
}

} // namespace

void DirectoryChangeWatcherTest::reportsChangesAnywhereInTheTree()
{
    QTemporaryDir root;
    QVERIFY(root.isValid());
    QVERIFY(QDir(root.path()).mkpath(QStringLiteral("nested/deeper")));

    DirectoryChangeWatcher watcher;
    QSignalSpy changed(&watcher, &DirectoryChangeWatcher::changed);
    QVERIFY(watcher.watch(root.path()));
    QVERIFY(watcher.watching());

    QVERIFY(writeFile(root.filePath(QStringLiteral("nested/deeper/file.txt")), "x"));
    QTRY_VERIFY_WITH_TIMEOUT(reportedPaths(changed).contains(QStringLiteral("nested/deeper/file.txt"))
                                 || reportedPaths(changed).contains(QStringLiteral("*")),
                             3000);
    QVERIFY(watcher.watching());

    // Stopped, it reports nothing more.
    watcher.stop();
    QVERIFY(!watcher.watching());
    changed.clear();
    QVERIFY(writeFile(root.filePath(QStringLiteral("top.txt")), "y"));
    QTest::qWait(300);
    QCOMPARE(changed.count(), 0);
}

void DirectoryChangeWatcherTest::stopsAndRefusesMissingDirectory()
{
    QTemporaryDir root;
    QVERIFY(root.isValid());
    DirectoryChangeWatcher watcher;
    QVERIFY(watcher.watch(root.path()));
    QVERIFY(!watcher.watch(root.filePath(QStringLiteral("missing"))));
    QVERIFY(!watcher.watching());
    QVERIFY(!watcher.watch(QString()));
}

QTEST_GUILESS_MAIN(DirectoryChangeWatcherTest)

#include "directory_change_watcher_test.moc"
