#include "git/git_repository_service.h"

#include <QSignalSpy>
#include <QTest>
#include <QVariantList>
#include <QVariantMap>

class GitItemModelTest final : public QObject {
    Q_OBJECT

private slots:
    void identicalReplaceIsNoop();
    void payloadChangeEmitsDataChangedOnly();
    void insertMiddleKeepsNeighbors();
    void removeOneEmitsRowsRemoved();
    void disjointListsReset();
    void reorderResets();
    void unkeyedDiffLinesResetOnCountChange();
    void duplicatePathResets();
    void emptyToItemsInserts();
    void itemsToEmptyRemoves();
    void prependInsertsAboveAndReindexesHashes();
};

namespace {

QVariantMap makeFileItem(const QString &path, int additions = 0, int deletions = 0,
                         const QString &status = QStringLiteral("modified"))
{
    return {
        {QStringLiteral("path"), path},
        {QStringLiteral("additions"), additions},
        {QStringLiteral("deletions"), deletions},
        {QStringLiteral("status"), status}
    };
}

QVariantMap makeCommit(const QString &hash, const QString &refs = {})
{
    return {{QStringLiteral("hash"), hash}, {QStringLiteral("refs"), refs}};
}

} // namespace

void GitItemModelTest::identicalReplaceIsNoop()
{
    GitItemModel model;
    const QVariantList list = {
        makeFileItem(QStringLiteral("a.txt"), 1, 0),
        makeFileItem(QStringLiteral("b.txt"), 2, 1)
    };
    QVERIFY(model.replace(list));

    QSignalSpy resetSpy(&model, &QAbstractItemModel::modelReset);
    QSignalSpy insertedSpy(&model, &QAbstractItemModel::rowsInserted);
    QSignalSpy removedSpy(&model, &QAbstractItemModel::rowsRemoved);
    QSignalSpy dataSpy(&model, &QAbstractItemModel::dataChanged);
    QSignalSpy countSpy(&model, &GitItemModel::countChanged);

    QVERIFY(!model.replace(list));
    QCOMPARE(resetSpy.count(), 0);
    QCOMPARE(insertedSpy.count(), 0);
    QCOMPARE(removedSpy.count(), 0);
    QCOMPARE(dataSpy.count(), 0);
    QCOMPARE(countSpy.count(), 0);
    QCOMPARE(model.count(), 2);
}

void GitItemModelTest::payloadChangeEmitsDataChangedOnly()
{
    GitItemModel model;
    const QVariantList initial = {
        makeFileItem(QStringLiteral("a.txt"), 1, 0),
        makeFileItem(QStringLiteral("b.txt"), 2, 1)
    };
    QVERIFY(model.replace(initial));

    QSignalSpy resetSpy(&model, &QAbstractItemModel::modelReset);
    QSignalSpy insertedSpy(&model, &QAbstractItemModel::rowsInserted);
    QSignalSpy removedSpy(&model, &QAbstractItemModel::rowsRemoved);
    QSignalSpy dataSpy(&model, &QAbstractItemModel::dataChanged);
    QSignalSpy countSpy(&model, &GitItemModel::countChanged);

    const QVariantList updated = {
        makeFileItem(QStringLiteral("a.txt"), 2, 0),
        makeFileItem(QStringLiteral("b.txt"), 2, 1)
    };
    QVERIFY(model.replace(updated));
    QCOMPARE(resetSpy.count(), 0);
    QCOMPARE(insertedSpy.count(), 0);
    QCOMPARE(removedSpy.count(), 0);
    QCOMPARE(dataSpy.count(), 1);
    QCOMPARE(countSpy.count(), 0);

    const QList<QVariant> signalArgs = dataSpy.takeFirst();
    const QModelIndex topLeft = signalArgs.at(0).toModelIndex();
    const QModelIndex bottomRight = signalArgs.at(1).toModelIndex();
    QCOMPARE(topLeft.row(), 0);
    QCOMPARE(bottomRight.row(), 0);

    const QVector<int> roles = signalArgs.at(2).value<QVector<int>>();
    QVERIFY(roles.contains(GitItemModel::ItemRole));
    QCOMPARE(model.itemAt(0).value(QStringLiteral("additions")).toInt(), 2);
}

void GitItemModelTest::insertMiddleKeepsNeighbors()
{
    GitItemModel model;
    const QVariantList initial = {
        makeFileItem(QStringLiteral("a.txt")),
        makeFileItem(QStringLiteral("c.txt"))
    };
    QVERIFY(model.replace(initial));

    QSignalSpy resetSpy(&model, &QAbstractItemModel::modelReset);
    QSignalSpy insertedSpy(&model, &QAbstractItemModel::rowsInserted);
    QSignalSpy removedSpy(&model, &QAbstractItemModel::rowsRemoved);
    QSignalSpy dataSpy(&model, &QAbstractItemModel::dataChanged);
    QSignalSpy countSpy(&model, &GitItemModel::countChanged);

    const QVariantList next = {
        makeFileItem(QStringLiteral("a.txt")),
        makeFileItem(QStringLiteral("b.txt")),
        makeFileItem(QStringLiteral("c.txt"))
    };
    QVERIFY(model.replace(next));
    QCOMPARE(resetSpy.count(), 0);
    QCOMPARE(insertedSpy.count(), 1);
    QCOMPARE(removedSpy.count(), 0);
    QCOMPARE(countSpy.count(), 1);

    const QList<QVariant> args = insertedSpy.takeFirst();
    QCOMPARE(args.at(1).toInt(), 1);
    QCOMPARE(args.at(2).toInt(), 1);

    QCOMPARE(model.count(), 3);
    QCOMPARE(model.itemAt(0).value(QStringLiteral("path")).toString(), QStringLiteral("a.txt"));
    QCOMPARE(model.itemAt(1).value(QStringLiteral("path")).toString(), QStringLiteral("b.txt"));
    QCOMPARE(model.itemAt(2).value(QStringLiteral("path")).toString(), QStringLiteral("c.txt"));
}

void GitItemModelTest::removeOneEmitsRowsRemoved()
{
    GitItemModel model;
    const QVariantList initial = {
        makeFileItem(QStringLiteral("a.txt")),
        makeFileItem(QStringLiteral("b.txt")),
        makeFileItem(QStringLiteral("c.txt"))
    };
    QVERIFY(model.replace(initial));

    QSignalSpy resetSpy(&model, &QAbstractItemModel::modelReset);
    QSignalSpy insertedSpy(&model, &QAbstractItemModel::rowsInserted);
    QSignalSpy removedSpy(&model, &QAbstractItemModel::rowsRemoved);
    QSignalSpy dataSpy(&model, &QAbstractItemModel::dataChanged);
    QSignalSpy countSpy(&model, &GitItemModel::countChanged);

    const QVariantList next = {
        makeFileItem(QStringLiteral("a.txt")),
        makeFileItem(QStringLiteral("c.txt"))
    };
    QVERIFY(model.replace(next));
    QCOMPARE(resetSpy.count(), 0);
    QCOMPARE(insertedSpy.count(), 0);
    QCOMPARE(removedSpy.count(), 1);
    QCOMPARE(countSpy.count(), 1);

    const QList<QVariant> args = removedSpy.takeFirst();
    QCOMPARE(args.at(1).toInt(), 1);
    QCOMPARE(args.at(2).toInt(), 1);

    QCOMPARE(model.count(), 2);
    QCOMPARE(model.itemAt(0).value(QStringLiteral("path")).toString(), QStringLiteral("a.txt"));
    QCOMPARE(model.itemAt(1).value(QStringLiteral("path")).toString(), QStringLiteral("c.txt"));
}

void GitItemModelTest::disjointListsReset()
{
    GitItemModel model;
    const QVariantList initial = {
        makeFileItem(QStringLiteral("a.txt")),
        makeFileItem(QStringLiteral("b.txt"))
    };
    QVERIFY(model.replace(initial));

    QSignalSpy resetSpy(&model, &QAbstractItemModel::modelReset);

    const QVariantList next = {
        makeFileItem(QStringLiteral("x.txt")),
        makeFileItem(QStringLiteral("y.txt"))
    };
    QVERIFY(model.replace(next));
    QVERIFY(resetSpy.count() >= 1);
    QCOMPARE(model.count(), 2);
    QCOMPARE(model.itemAt(0).value(QStringLiteral("path")).toString(), QStringLiteral("x.txt"));
    QCOMPARE(model.itemAt(1).value(QStringLiteral("path")).toString(), QStringLiteral("y.txt"));
}

void GitItemModelTest::reorderResets()
{
    GitItemModel model;
    const QVariantList initial = {
        makeFileItem(QStringLiteral("a.txt")),
        makeFileItem(QStringLiteral("b.txt"))
    };
    QVERIFY(model.replace(initial));

    QSignalSpy resetSpy(&model, &QAbstractItemModel::modelReset);

    const QVariantList next = {
        makeFileItem(QStringLiteral("b.txt")),
        makeFileItem(QStringLiteral("a.txt"))
    };
    QVERIFY(model.replace(next));
    QVERIFY(resetSpy.count() >= 1);
    QCOMPARE(model.count(), 2);
    QCOMPARE(model.itemAt(0).value(QStringLiteral("path")).toString(), QStringLiteral("b.txt"));
    QCOMPARE(model.itemAt(1).value(QStringLiteral("path")).toString(), QStringLiteral("a.txt"));
}

void GitItemModelTest::unkeyedDiffLinesResetOnCountChange()
{
    GitItemModel model;
    const QVariantList initial = {
        QVariantMap{{QStringLiteral("kind"), QStringLiteral("context")}, {QStringLiteral("oldText"), QStringLiteral("line 1")}},
        QVariantMap{{QStringLiteral("kind"), QStringLiteral("context")}, {QStringLiteral("oldText"), QStringLiteral("line 2")}}
    };
    QVERIFY(model.replace(initial));

    QSignalSpy resetSpy(&model, &QAbstractItemModel::modelReset);

    const QVariantList next = {
        QVariantMap{{QStringLiteral("kind"), QStringLiteral("context")}, {QStringLiteral("oldText"), QStringLiteral("line 1")}},
        QVariantMap{{QStringLiteral("kind"), QStringLiteral("context")}, {QStringLiteral("oldText"), QStringLiteral("line 2")}},
        QVariantMap{{QStringLiteral("kind"), QStringLiteral("add")}, {QStringLiteral("newText"), QStringLiteral("line 3")}}
    };
    QVERIFY(model.replace(next));
    QVERIFY(resetSpy.count() >= 1);
    QCOMPARE(model.count(), 3);
}

void GitItemModelTest::duplicatePathResets()
{
    GitItemModel model;
    const QVariantList initial = {
        makeFileItem(QStringLiteral("a.txt")),
        makeFileItem(QStringLiteral("b.txt"))
    };
    QVERIFY(model.replace(initial));

    QSignalSpy resetSpy(&model, &QAbstractItemModel::modelReset);

    const QVariantList duplicateList = {
        makeFileItem(QStringLiteral("a.txt")),
        makeFileItem(QStringLiteral("a.txt"))
    };
    QVERIFY(model.replace(duplicateList));
    QVERIFY(resetSpy.count() >= 1);
    QCOMPARE(model.count(), 2);
}

void GitItemModelTest::emptyToItemsInserts()
{
    GitItemModel model;
    QSignalSpy resetSpy(&model, &QAbstractItemModel::modelReset);
    QSignalSpy insertedSpy(&model, &QAbstractItemModel::rowsInserted);
    QSignalSpy removedSpy(&model, &QAbstractItemModel::rowsRemoved);
    QSignalSpy countSpy(&model, &GitItemModel::countChanged);

    const QVariantList list = {
        makeFileItem(QStringLiteral("a.txt"))
    };
    QVERIFY(model.replace(list));
    QCOMPARE(resetSpy.count(), 0);
    QCOMPARE(insertedSpy.count(), 1);
    QCOMPARE(removedSpy.count(), 0);
    QCOMPARE(countSpy.count(), 1);
    QCOMPARE(model.count(), 1);
}

void GitItemModelTest::itemsToEmptyRemoves()
{
    GitItemModel model;
    const QVariantList list = {
        makeFileItem(QStringLiteral("a.txt"))
    };
    QVERIFY(model.replace(list));

    QSignalSpy resetSpy(&model, &QAbstractItemModel::modelReset);
    QSignalSpy insertedSpy(&model, &QAbstractItemModel::rowsInserted);
    QSignalSpy removedSpy(&model, &QAbstractItemModel::rowsRemoved);
    QSignalSpy countSpy(&model, &GitItemModel::countChanged);

    QVERIFY(model.replace({}));
    QCOMPARE(resetSpy.count(), 0);
    QCOMPARE(insertedSpy.count(), 0);
    QCOMPARE(removedSpy.count(), 1);
    QCOMPARE(countSpy.count(), 1);
    QCOMPARE(model.count(), 0);
}

void GitItemModelTest::prependInsertsAboveAndReindexesHashes()
{
    GitItemModel model;
    model.append({makeCommit(QStringLiteral("c2")), makeCommit(QStringLiteral("c1"))});
    QCOMPARE(model.rowOfHash(QStringLiteral("c1")), 1);
    model.append({makeCommit(QStringLiteral("c0"))});
    QCOMPARE(model.rowOfHash(QStringLiteral("c0")), 2);

    QSignalSpy insertedSpy(&model, &QAbstractItemModel::rowsInserted);
    QSignalSpy resetSpy(&model, &QAbstractItemModel::modelReset);
    QSignalSpy dataSpy(&model, &QAbstractItemModel::dataChanged);
    model.prepend({makeCommit(QStringLiteral("c4")), makeCommit(QStringLiteral("c3"))});
    QCOMPARE(resetSpy.count(), 0);
    QCOMPARE(insertedSpy.count(), 1);
    QCOMPARE(insertedSpy.at(0).at(1).toInt(), 0);
    QCOMPARE(insertedSpy.at(0).at(2).toInt(), 1);
    QCOMPARE(model.count(), 5);
    QCOMPARE(model.rowOfHash(QStringLiteral("c4")), 0);
    QCOMPARE(model.rowOfHash(QStringLiteral("c1")), 3);
    QCOMPARE(model.rowOfHash(QStringLiteral("missing")), -1);

    // A row's payload changes in place; an identical payload is no change.
    model.setItem(2, makeCommit(QStringLiteral("c2"), QStringLiteral("tag: v1")));
    model.setItem(2, makeCommit(QStringLiteral("c2"), QStringLiteral("tag: v1")));
    QCOMPARE(dataSpy.count(), 1);
    QCOMPARE(model.itemAt(2).value(QStringLiteral("refs")).toString(), QStringLiteral("tag: v1"));
    QCOMPARE(model.rowOfHash(QStringLiteral("c2")), 2);
}

QTEST_MAIN(GitItemModelTest)
#include "git_item_model_test.moc"
