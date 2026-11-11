#include "terminal/render/terminal_ime_composition_state.h"

#include <QFontDatabase>
#include <QTest>

using namespace span::terminal;

namespace {

TerminalImeAnchor anchorAt(int row, int column) {
    Cursor cursor;
    cursor.row = row;
    cursor.column = column;
    cursor.visible = false;
    return {cursor, TerminalImeAnchorSource::ReverseVideoCell};
}

} // namespace

class TerminalImeCompositionStateTest final : public QObject {
    Q_OBJECT

private slots:
    void defaultsCursorToPreeditEnd();
    void normalizesCursorAndTextFormats();
    void preservesSurrogateBoundaries();
    void retainsAnchorOnlyWithinSameLiveEpoch();
    void freezesAnchorForCompleteComposition();
    void suppressesFrameQueriesWhileCompositionAnchorIsStable();
    void layoutTracksInternalCursorAcrossCjkText();
    void emptyPreeditClearsAllState();
};

void TerminalImeCompositionStateTest::defaultsCursorToPreeditEnd() {
    TerminalImeCompositionState state;
    state.update(QStringLiteral("pinyin"), {});
    QVERIFY(state.active());
    QCOMPARE(state.cursorPosition(), 6);
    QVERIFY(state.cursorVisible());
    QVERIFY(!state.cursorColor().isValid());
}

void TerminalImeCompositionStateTest::normalizesCursorAndTextFormats() {
    QTextCharFormat format;
    format.setForeground(QColor(Qt::red));
    format.setBackground(QColor(Qt::yellow));
    format.setFontUnderline(true);
    QList<QInputMethodEvent::Attribute> attributes;
    attributes.append({QInputMethodEvent::TextFormat, -2, 5,
                       QVariant::fromValue(QTextFormat(format))});
    attributes.append({QInputMethodEvent::Cursor, 2, 0, QColor(Qt::green)});

    TerminalImeCompositionState state;
    state.update(QStringLiteral("abcd"), attributes);
    QCOMPARE(state.cursorPosition(), 2);
    QVERIFY(!state.cursorVisible());
    QCOMPARE(state.cursorColor(), QColor(Qt::green));
    QCOMPARE(state.formats().size(), 1);
    QCOMPARE(state.formats().first().start, 0);
    QCOMPARE(state.formats().first().length, 3);
    QCOMPARE(state.formats().first().format.foreground().color(), QColor(Qt::red));
    QCOMPARE(state.layoutFormats().size(), 1);
}

void TerminalImeCompositionStateTest::preservesSurrogateBoundaries() {
    const QString text = QStringLiteral("a\U0001F642b");
    TerminalImeCompositionState state;
    state.update(text, {{QInputMethodEvent::Cursor, 2, 1, {}}});
    QCOMPARE(text.size(), 4);
    QCOMPARE(state.cursorPosition(), 3);

    QTextCharFormat format;
    format.setFontUnderline(true);
    state.update(text, {{QInputMethodEvent::TextFormat, 2, 1,
                         QVariant::fromValue(QTextFormat(format))}});
    QCOMPARE(state.formats().first().start, 1);
    QCOMPARE(state.formats().first().length, 2);
}

void TerminalImeCompositionStateTest::retainsAnchorOnlyWithinSameLiveEpoch() {
    TerminalImeCompositionState state;
    state.update(QStringLiteral("pin"), {});
    QVERIFY(state.reanchor(anchorAt(2, 4), 10, 7, true, 80, 24));
    QCOMPARE(state.anchor().cursor.row, 2);
    QCOMPARE(state.anchorFrameSequence(), quint64(10));

    QVERIFY(!state.reanchor({}, 11, 7, true, 80, 24));
    QCOMPARE(state.anchor().cursor.column, 4);
    QCOMPARE(state.anchorFrameSequence(), quint64(10));

    QVERIFY(state.reanchor({}, 12, 8, true, 80, 24));
    QVERIFY(!state.anchor().isValid());
    QVERIFY(state.reanchor(anchorAt(1, 2), 13, 8, true, 80, 24));
    QVERIFY(state.reanchor({}, 14, 8, false, 80, 24));
    QVERIFY(!state.anchor().isValid());
}

void TerminalImeCompositionStateTest::freezesAnchorForCompleteComposition() {
    TerminalImeCompositionState state;
    state.update(QStringLiteral("pin"), {});
    QVERIFY(state.reanchor(anchorAt(2, 4), 10, 7, true, 80, 24));

    QVERIFY(!state.reanchor(anchorAt(8, 30), 11, 7, true, 80, 24));
    QCOMPARE(state.anchor().cursor.row, 2);
    QCOMPARE(state.anchor().cursor.column, 4);
    QCOMPARE(state.anchorFrameSequence(), quint64(10));
}

void TerminalImeCompositionStateTest::suppressesFrameQueriesWhileCompositionAnchorIsStable() {
    QCOMPARE(TerminalImeCompositionState::frameQueryChanges(
                 true, false, true, false), Qt::InputMethodQueries{});
    QCOMPARE(TerminalImeCompositionState::frameQueryChanges(
                 true, true, true, false),
             Qt::InputMethodQueries(Qt::ImCursorRectangle
                                    | Qt::ImAnchorRectangle));
    QVERIFY(TerminalImeCompositionState::frameQueryChanges(
                false, false, true, false).testFlag(Qt::ImCursorRectangle));
    QVERIFY(TerminalImeCompositionState::frameQueryChanges(
                true, false, false, true)
                .testFlag(Qt::ImInputItemClipRectangle));
}

void TerminalImeCompositionStateTest::layoutTracksInternalCursorAcrossCjkText() {
    TerminalImeCompositionState start;
    start.update(QStringLiteral("拼音abc"),
                 {{QInputMethodEvent::Cursor, 0, 1, {}}});
    TerminalImeCompositionState middle;
    middle.update(QStringLiteral("拼音abc"),
                  {{QInputMethodEvent::Cursor, 2, 1, {}}});
    TerminalImeCompositionState end;
    end.update(QStringLiteral("拼音abc"), {});
    const QFont font = QFontDatabase::systemFont(QFontDatabase::FixedFont);
    const auto startMetrics = start.layoutMetrics(font);
    const auto middleMetrics = middle.layoutMetrics(font);
    const auto endMetrics = end.layoutMetrics(font);
    QCOMPARE(startMetrics.cursorX, 0.0);
    QVERIFY(middleMetrics.cursorX > startMetrics.cursorX);
    QVERIFY(endMetrics.cursorX > middleMetrics.cursorX);
    QVERIFY(endMetrics.textWidth > 0.0);
}

void TerminalImeCompositionStateTest::emptyPreeditClearsAllState() {
    TerminalImeCompositionState state;
    state.update(QStringLiteral("pin"), {});
    state.reanchor(anchorAt(1, 3), 2, 4, true, 20, 5);
    state.update(QString(), {});
    QVERIFY(!state.active());
    QVERIFY(!state.anchor().isValid());
    QCOMPARE(state.anchorTransportEpoch(), quint64(0));
}

QTEST_MAIN(TerminalImeCompositionStateTest)

#include "terminal_ime_composition_state_test.moc"
