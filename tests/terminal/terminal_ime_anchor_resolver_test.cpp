#include "terminal/render/terminal_ime_anchor_resolver.h"

#include <QTest>

using namespace span::terminal;

namespace {

Frame emptyFrame(int columns = 8, int rows = 3) {
    Frame frame;
    frame.columns = columns;
    frame.rows = rows;
    frame.lines.resize(rows);
    for (Line &line : frame.lines) {
        line.cells.resize(columns);
        for (Cell &cell : line.cells) {
            cell.text = QStringLiteral(" ");
        }
    }
    return frame;
}

void setCursor(Frame &frame, int row, int column, bool visible = false) {
    frame.cursor.row = row;
    frame.cursor.column = column;
    frame.cursor.visible = visible;
}

void setReverseCell(Frame &frame, int row, int column) {
    frame.lines[row].cells[column].style.reverse = true;
}

} // namespace

class TerminalImeAnchorResolverTest final : public QObject {
    Q_OBJECT

private slots:
    void hiddenCursorUsesUniqueReverseCell();
    void hiddenCursorAcceptsOneWideLogicalCell();
    void ambiguousReverseCellsFallBackToProtocolCursor();
    void longReverseRunAndNoCandidateFallBackToProtocolCursor();
    void visibleProtocolCursorAlwaysWins();
    void invalidLifecycleContextHasNoAnchor();
};

void TerminalImeAnchorResolverTest::hiddenCursorUsesUniqueReverseCell() {
    Frame frame = emptyFrame();
    setReverseCell(frame, 1, 2);

    setCursor(frame, 2, 7);
    const TerminalCursorResolution resolution = TerminalCursorResolver::resolve(frame, 7, true);
    const TerminalImeAnchor anchor = resolution.imeAnchor;
    QVERIFY(!resolution.visualCursor);
    QVERIFY(anchor.isValid());
    QCOMPARE(anchor.source, TerminalImeAnchorSource::ReverseVideoCell);
    QCOMPARE(anchor.cursor.row, 1);
    QCOMPARE(anchor.cursor.column, 2);
}

void TerminalImeAnchorResolverTest::hiddenCursorAcceptsOneWideLogicalCell() {
    Frame frame = emptyFrame();
    Cell &wide = frame.lines[0].cells[3];
    wide.text = QStringLiteral("界");
    wide.width = 2;
    wide.style.reverse = true;
    frame.lines[0].cells[4].continuation = true;
    frame.lines[0].cells[4].width = 0;

    setCursor(frame, 2, 6);
    const TerminalCursorResolution resolution = TerminalCursorResolver::resolve(frame, 7, true);
    const TerminalImeAnchor anchor = resolution.imeAnchor;
    QVERIFY(!resolution.visualCursor);
    QCOMPARE(anchor.source, TerminalImeAnchorSource::ReverseVideoCell);
    QCOMPARE(anchor.cursor.row, 0);
    QCOMPARE(anchor.cursor.column, 3);
}

void TerminalImeAnchorResolverTest::ambiguousReverseCellsFallBackToProtocolCursor() {
    Frame frame = emptyFrame();
    setReverseCell(frame, 0, 1);
    setReverseCell(frame, 2, 5);

    setCursor(frame, 1, 4);
    const TerminalCursorResolution resolution = TerminalCursorResolver::resolve(frame, 7, true);
    QVERIFY(!resolution.imeAnchor.isValid());
    QVERIFY(!resolution.visualCursor);
}

void TerminalImeAnchorResolverTest::longReverseRunAndNoCandidateFallBackToProtocolCursor() {
    Frame frame = emptyFrame();
    setReverseCell(frame, 1, 2);
    setReverseCell(frame, 1, 3);
    setCursor(frame, 2, 6);
    TerminalImeAnchor anchor = TerminalCursorResolver::resolve(frame, 7, true).imeAnchor;
    QVERIFY(!anchor.isValid());

    frame = emptyFrame();
    setCursor(frame, 2, 6);
    anchor = TerminalCursorResolver::resolve(frame, 7, true).imeAnchor;
    QVERIFY(!anchor.isValid());
}

void TerminalImeAnchorResolverTest::visibleProtocolCursorAlwaysWins() {
    Frame frame = emptyFrame();
    setReverseCell(frame, 0, 1);

    setCursor(frame, 2, 5, true);
    const TerminalCursorResolution resolution = TerminalCursorResolver::resolve(frame, 7, true);
    const TerminalImeAnchor anchor = resolution.imeAnchor;
    QVERIFY(resolution.visualCursor);
    QCOMPARE(resolution.visualCursor->row, 2);
    QCOMPARE(resolution.visualCursor->column, 5);
    QCOMPARE(anchor.source, TerminalImeAnchorSource::ProtocolCursor);
    QCOMPARE(anchor.cursor.row, 2);
    QCOMPARE(anchor.cursor.column, 5);
}

void TerminalImeAnchorResolverTest::invalidLifecycleContextHasNoAnchor() {
    Frame frame = emptyFrame();
    setCursor(frame, 1, 1, true);
    QVERIFY(!TerminalCursorResolver::resolve(frame, 0, true).imeAnchor.isValid());
    // A view scrolled into history shows no live cursor.
    QVERIFY(!TerminalCursorResolver::resolve(frame, 7, false).imeAnchor.isValid());
    QVERIFY(!TerminalCursorResolver::resolve(frame, 7, false).visualCursor);
    setCursor(frame, 3, 1, true);
    QVERIFY(!TerminalCursorResolver::resolve(frame, 7, true).imeAnchor.isValid());
    setCursor(frame, 1, 8, true);
    QVERIFY(!TerminalCursorResolver::resolve(frame, 7, true).imeAnchor.isValid());
}

QTEST_MAIN(TerminalImeAnchorResolverTest)

#include "terminal_ime_anchor_resolver_test.moc"
