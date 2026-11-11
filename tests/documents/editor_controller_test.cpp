#include "documents/editor_controller.h"

#include <QSignalSpy>
#include <QTest>

namespace {

QString gitStatus(EditorController &controller, int oneBasedLine)
{
    CodePreviewLineModel *model = controller.lineModel();
    return model->data(model->index(oneBasedLine - 1), CodePreviewLineModel::GitStatusRole).toString();
}

bool deletionBefore(EditorController &controller, int oneBasedLine)
{
    CodePreviewLineModel *model = controller.lineModel();
    return model->data(model->index(oneBasedLine - 1), CodePreviewLineModel::DeletionBeforeRole).toBool();
}

// Git status of every row, top to bottom.
QStringList gitStatuses(EditorController &controller)
{
    QStringList statuses;
    for (int line = 1; line <= controller.lineModel()->count(); ++line) statuses.append(gitStatus(controller, line));
    return statuses;
}

// One-based lines that show a deletion marker above them.
QList<int> linesWithDeletionBefore(EditorController &controller)
{
    QList<int> lines;
    for (int line = 1; line <= controller.lineModel()->count(); ++line) {
        if (deletionBefore(controller, line)) lines.append(line);
    }
    return lines;
}

} // namespace

class EditorControllerTest final : public QObject {
    Q_OBJECT
private slots:
    void tracksGitLinesAgainstBaseContent_data();
    void tracksGitLinesAgainstBaseContent();
    void cursorAndNavigationStayInBounds();
    void supportsSearchAndGoToLine();
    void normalizesCrlfInTextAndBaseContent();
    void linearHighlightLinesAndLineForOffset();
    void clearBaseContentResetsGitDiff();
    void handlesInvalidRegexAndZeroLengthMatchesSafely();
    void lineModelExposesRowsAndWidestLine();
    void gitRebuildReportsOnlyChangedRows();
    void unchangedGitRebuildEmitsNothing();
    void noBaseMeansNoGitStateAndNoRebuild();
    void clearDocumentLeavesNoRows();
};

void EditorControllerTest::tracksGitLinesAgainstBaseContent_data()
{
    QTest::addColumn<QString>("base");
    QTest::addColumn<QString>("text");
    QTest::addColumn<bool>("existsInHead");
    QTest::addColumn<QStringList>("statuses"); // one per line of `text`, "" for unchanged
    QTest::addColumn<QList<int>>("deletedBefore");

    QTest::newRow("untracked file") << QString() << QStringLiteral("a\nb") << false << QStringList{"added", "added"}
                                    << QList<int>{};
    QTest::newRow("modified line") << QStringLiteral("a\nb\nc") << QStringLiteral("a\nB\nc") << true
                                   << QStringList{"", "modified", ""} << QList<int>{};
    QTest::newRow("inserted block") << QStringLiteral("a\nc") << QStringLiteral("a\nb1\nb2\nc") << true
                                    << QStringList{"", "added", "added", ""} << QList<int>{};
    QTest::newRow("deleted line") << QStringLiteral("a\nb\nc") << QStringLiteral("a\nc") << true << QStringList{"", ""}
                                  << QList<int>{2};
    QTest::newRow("appended lines") << QStringLiteral("a") << QStringLiteral("a\nb\nc") << true
                                    << QStringList{"", "added", "added"} << QList<int>{};
    QTest::newRow("truncated tail") << QStringLiteral("a\nb\nc") << QStringLiteral("a") << true << QStringList{""}
                                    << QList<int>{1};
}

void EditorControllerTest::tracksGitLinesAgainstBaseContent()
{
    QFETCH(QString, base);
    QFETCH(QString, text);
    QFETCH(bool, existsInHead);
    QFETCH(QStringList, statuses);
    QFETCH(QList<int>, deletedBefore);

    EditorController controller;
    controller.setGitDiffDelayForTesting(1);
    controller.setText(text);
    QSignalSpy rebuilt(controller.lineModel(), &QAbstractItemModel::dataChanged);
    controller.setBaseContent(base, existsInHead);
    QTRY_VERIFY(rebuilt.count() >= 1);
    QCOMPARE(gitStatuses(controller), statuses);
    QCOMPARE(linesWithDeletionBefore(controller), deletedBefore);
}

void EditorControllerTest::cursorAndNavigationStayInBounds()
{
    EditorController controller;
    QSignalSpy cursor(&controller, &EditorController::cursorInfoChanged);
    controller.setCursorInfo(3, 4, 2);
    QCOMPARE(controller.cursorLine(), 3);
    controller.setCursorInfo(0, -3, -5);
    QCOMPARE(controller.cursorLine(), 1);
    QCOMPARE(controller.cursorColumn(), 1);
    QCOMPARE(controller.selectionLength(), 0);
    controller.setCursorInfo(1, 1, 0);
    QCOMPARE(cursor.count(), 2);

    QVERIFY(controller.nextMatch(0, false).isEmpty());
    controller.setText(QStringLiteral("a b a"));
    controller.setSearch(QStringLiteral("a"), true, false, false);
    QCOMPARE(controller.nextMatch(4, true).value(QStringLiteral("start")).toInt(), 0);
    // Searching backwards past the first match wraps to the last.
    QCOMPARE(controller.nextMatch(0, true).value(QStringLiteral("start")).toInt(), 4);
    // Editing the text reruns the active search.
    controller.setText(QStringLiteral("a a a a"));
    QCOMPARE(controller.matchCount(), 4);
    QCOMPARE(controller.goToLine(99), 7);
}

void EditorControllerTest::supportsSearchAndGoToLine()
{
    EditorController controller;
    controller.setText(QStringLiteral("word {\n  word\n}\n"));
    controller.setSearch("word", true, true, false);
    QCOMPARE(controller.matchCount(), 2);
    const QVariantMap initialMatch = controller.nextMatch(-1, false);
    QCOMPARE(initialMatch.value("start").toInt(), 0);
    const QVariantMap match = controller.nextMatch(0, false);
    QCOMPARE(match.value("start").toInt(), 9);
    const QVariantMap lastMatch = controller.nextMatch(-1, true);
    QCOMPARE(lastMatch.value("start").toInt(), 9);
    QCOMPARE(controller.goToLine(2), 7);
    QCOMPARE(controller.lineCount(), 4);
}

void EditorControllerTest::normalizesCrlfInTextAndBaseContent()
{
    EditorController controller;
    controller.setGitDiffDelayForTesting(1);
    QSignalSpy changed(controller.lineModel(), &QAbstractItemModel::dataChanged);
    controller.setText(QStringLiteral("line1\r\nline2\r\nline3\r\n"));
    controller.setBaseContent(QStringLiteral("line1\r\nline2\r\nline3\r\n"), true);
    QCOMPARE(controller.lineModel()->lineText(1), QStringLiteral("line1"));

    controller.setText(QStringLiteral("line1\nline2\nline3\n"));
    controller.setBaseContent(QStringLiteral("line1\r\nline2\r\nline3\r\n"), true);
    // Normalized text equals the base each time: no row is ever marked.
    QTest::qWait(50);
    QCOMPARE(changed.count(), 0);
    // Three text lines plus the empty one after the final newline.
    QCOMPARE(gitStatuses(controller), QStringList(4, QString()));
}

void EditorControllerTest::linearHighlightLinesAndLineForOffset()
{
    EditorController controller;
    const QString text = QStringLiteral("alpha\nbeta\ngamma\nalpha\nomega\n");
    controller.setText(text);
    controller.setSearch(QStringLiteral("alpha"), true, false, false);
    QCOMPARE(controller.matchCount(), 2);
    const QVariantMap highlights = controller.highlightLines();
    QCOMPARE(highlights.value(QStringLiteral("1")).toBool(), true);
    QCOMPARE(highlights.value(QStringLiteral("4")).toBool(), true);
    QCOMPARE(highlights.value(QStringLiteral("2")).toBool(), false);

    QCOMPARE(controller.lineForOffset(0), 1);
    QCOMPARE(controller.lineForOffset(6), 2);
    QCOMPARE(controller.lineForOffset(18), 4);
}

void EditorControllerTest::clearBaseContentResetsGitDiff()
{
    EditorController controller;
    controller.setGitDiffDelayForTesting(1);
    controller.setText(QStringLiteral("new file\n"));
    controller.setBaseContent(QStringLiteral("old file\n"), true);
    QTRY_COMPARE(gitStatus(controller, 1), QString("modified"));
    QSignalSpy reset(controller.lineModel(), &QAbstractItemModel::modelReset);
    QSignalSpy changed(controller.lineModel(), &QAbstractItemModel::dataChanged);
    controller.clearBaseContent();
    // Synchronous and row-level: the marked row is cleared, nothing rebuilt.
    QVERIFY(gitStatus(controller, 1).isEmpty());
    QCOMPARE(reset.count(), 0);
    QCOMPARE(changed.count(), 1);
    QCOMPARE(changed.at(0).at(0).value<QModelIndex>().row(), 0);
    QCOMPARE(changed.at(0).at(1).value<QModelIndex>().row(), 0);
}

void EditorControllerTest::handlesInvalidRegexAndZeroLengthMatchesSafely()
{
    EditorController controller;
    controller.setText(QStringLiteral("alpha beta gamma\n"));

    // Invalid regex: should not crash, searchValid becomes false, matches 0
    controller.setSearch(QStringLiteral("[invalid("), true, false, true);
    QVERIFY(!controller.searchValid());
    QCOMPARE(controller.matchCount(), 0);

    // Valid regex with zero-width matches (e.g. ^ or \b): filtered out
    controller.setSearch(QStringLiteral("^"), true, false, true);
    QVERIFY(controller.searchValid());
    QCOMPARE(controller.matchCount(), 0);

    // Valid normal regex
    controller.setSearch(QStringLiteral("\\b[a-z]{4}\\b"), true, false, true);
    QVERIFY(controller.searchValid());
    QCOMPARE(controller.matchCount(), 1); // "beta" has length 4
}

void EditorControllerTest::lineModelExposesRowsAndWidestLine()
{
    EditorController controller;
    CodePreviewLineModel *model = controller.lineModel();
    QSignalSpy reset(model, &QAbstractItemModel::modelReset);
    controller.setText(QStringLiteral("a\n\tbc\nlonger line"));
    QCOMPARE(reset.count(), 1);
    QCOMPARE(model->count(), 3);
    QCOMPARE(model->rowCount(), 3);
    QCOMPARE(model->data(model->index(1), CodePreviewLineModel::TextRole).toString(), QStringLiteral("\tbc"));
    QCOMPARE(model->lineText(3), QStringLiteral("longer line"));
    QVERIFY(model->lineText(4).isEmpty());
    QCOMPARE(model->maxColumns(), 11);
    // A tab advances to the next 4-column stop.
    controller.setText(QStringLiteral("ab\tc"));
    QCOMPARE(model->maxColumns(), 5);
    const QHash<int, QByteArray> roles = model->roleNames();
    QCOMPARE(roles.value(CodePreviewLineModel::TextRole), QByteArray("text"));
    QCOMPARE(roles.value(CodePreviewLineModel::GitStatusRole), QByteArray("gitStatus"));
    QCOMPARE(roles.value(CodePreviewLineModel::DeletionBeforeRole), QByteArray("deletionBefore"));
    // The same text again is no new document.
    controller.setText(QStringLiteral("ab\tc"));
    QCOMPARE(reset.count(), 2);
}

void EditorControllerTest::gitRebuildReportsOnlyChangedRows()
{
    EditorController controller;
    controller.setGitDiffDelayForTesting(1);
    controller.setText(QStringLiteral("a\nB\nc\nd\ne\nf"));
    controller.setBaseContent(QStringLiteral("a\nb\nc\nd\ne\nf"), true);
    QTRY_COMPARE(gitStatus(controller, 2), QStringLiteral("modified"));

    // Same text against a base that differs in other lines: row 2 clears,
    // rows 5 and 6 change, the rows between are untouched.
    QSignalSpy reset(controller.lineModel(), &QAbstractItemModel::modelReset);
    QSignalSpy changed(controller.lineModel(), &QAbstractItemModel::dataChanged);
    controller.setBaseContent(QStringLiteral("a\nB\nc\nd\nE\nF"), true);
    QTRY_COMPARE(changed.count(), 2);
    QCOMPARE(reset.count(), 0);
    QCOMPARE(changed.at(0).at(0).value<QModelIndex>().row(), 1);
    QCOMPARE(changed.at(0).at(1).value<QModelIndex>().row(), 1);
    QCOMPARE(changed.at(1).at(0).value<QModelIndex>().row(), 4);
    QCOMPARE(changed.at(1).at(1).value<QModelIndex>().row(), 5);
    const auto roles = changed.at(1).at(2).value<QList<int>>();
    QVERIFY(roles.contains(CodePreviewLineModel::GitStatusRole));
    QVERIFY(!roles.contains(CodePreviewLineModel::TextRole));
    QVERIFY(gitStatus(controller, 2).isEmpty());
    QCOMPARE(gitStatus(controller, 5), QStringLiteral("modified"));
    QCOMPARE(gitStatus(controller, 6), QStringLiteral("modified"));
}

void EditorControllerTest::unchangedGitRebuildEmitsNothing()
{
    EditorController controller;
    controller.setGitDiffDelayForTesting(1);
    controller.setText(QStringLiteral("a\nB\nc"));
    controller.setBaseContent(QStringLiteral("a\nb\nc"), true);
    QTRY_COMPARE(gitStatus(controller, 2), QStringLiteral("modified"));

    QSignalSpy reset(controller.lineModel(), &QAbstractItemModel::modelReset);
    QSignalSpy changed(controller.lineModel(), &QAbstractItemModel::dataChanged);
    // The same base again (a git status refresh) rebuilds to the same states.
    controller.setBaseContent(QStringLiteral("a\nb\nc"), true);
    QTest::qWait(50);
    QCOMPARE(changed.count(), 0);
    QCOMPARE(reset.count(), 0);
}

void EditorControllerTest::noBaseMeansNoGitStateAndNoRebuild()
{
    EditorController controller;
    controller.setGitDiffDelayForTesting(1);
    QSignalSpy changed(controller.lineModel(), &QAbstractItemModel::dataChanged);
    // Outside a repository no base ever arrives: nothing is marked added.
    controller.setText(QStringLiteral("a\nb"));
    QTest::qWait(50);
    QCOMPARE(changed.count(), 0);
    QVERIFY(gitStatus(controller, 1).isEmpty());
    QVERIFY(gitStatus(controller, 2).isEmpty());
}

void EditorControllerTest::clearDocumentLeavesNoRows()
{
    EditorController controller;
    controller.setText(QStringLiteral("a\nb"));
    controller.clearDocument();
    QCOMPARE(controller.lineModel()->count(), 0);
    QCOMPARE(controller.lineModel()->maxColumns(), 0);
    // An empty file is one empty row, even right after a cleared document.
    controller.setText(QString());
    QCOMPARE(controller.lineModel()->count(), 1);
}

QTEST_MAIN(EditorControllerTest)
#include "editor_controller_test.moc"
