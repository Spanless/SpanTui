#include "terminal/render/terminal_cursor_animation.h"
#include "terminal/terminal_constants.h"

#include <QTest>

using namespace span::terminal;

class TerminalCursorAnimationTest final : public QObject {
    Q_OBJECT

private slots:
    void breathingOpacityUsesFixedSineCycle();
    void holdWindowCoversFiveSeconds();
    void fadeTimerOnlyRunsInsideHoldWhenIdle();
    void resetsOnlyForSubstantivePresentedCursorChanges();
    void gatesAnimationToInteractiveLiveCursor();
};

void TerminalCursorAnimationTest::breathingOpacityUsesFixedSineCycle() {
    QCOMPARE(TerminalCursorAnimation::opacityAt(0), 1.0);
    QVERIFY(qAbs(TerminalCursorAnimation::opacityAt(265) - 0.5) < 0.0001);
    QCOMPARE(TerminalCursorAnimation::opacityAt(530), kCursorMinimumOpacity);
    QVERIFY(qAbs(TerminalCursorAnimation::opacityAt(795) - 0.5) < 0.0001);
    QCOMPARE(TerminalCursorAnimation::opacityAt(1060), 1.0);
}

void TerminalCursorAnimationTest::holdWindowCoversFiveSeconds() {
    QVERIFY(TerminalCursorAnimation::withinHold(0));
    QVERIFY(TerminalCursorAnimation::withinHold(4999));
    QVERIFY(!TerminalCursorAnimation::withinHold(5000));
    QVERIFY(!TerminalCursorAnimation::withinHold(-1));
}

void TerminalCursorAnimationTest::fadeTimerOnlyRunsInsideHoldWhenIdle() {
    QVERIFY(TerminalCursorAnimation::shouldRunFadeTimer(true, false, true));
    QVERIFY(!TerminalCursorAnimation::shouldRunFadeTimer(true, false, false));
    QVERIFY(!TerminalCursorAnimation::shouldRunFadeTimer(true, true, true));
    QVERIFY(!TerminalCursorAnimation::shouldRunFadeTimer(false, false, true));
}

void TerminalCursorAnimationTest::resetsOnlyForSubstantivePresentedCursorChanges() {
    Frame before;
    before.cursor = {2, 4, true, true, CursorShape::Block};
    Frame same = before;
    same.lines.resize(3);
    QVERIFY(!TerminalCursorAnimation::presentationChanged(before, same));
    same.cursor.shape = CursorShape::Bar;
    QVERIFY(TerminalCursorAnimation::presentationChanged(before, same));
    same = before;
    same.cursor.column = 5;
    QVERIFY(TerminalCursorAnimation::presentationChanged(before, same));
    QVERIFY(!TerminalCursorAnimation::shouldReset(before, same));
    same = before;
    same.cursor.visible = false;
    QVERIFY(TerminalCursorAnimation::shouldReset(before, same));
}

void TerminalCursorAnimationTest::gatesAnimationToInteractiveLiveCursor() {
    Cursor cursor;
    cursor.visible = true;
    cursor.blink = true;
    const std::optional<Cursor> visible = cursor;
    QVERIFY(TerminalCursorAnimation::shouldAnimate(
        visible, true, true, true, true, true, false));
    QVERIFY(!TerminalCursorAnimation::shouldAnimate(
        visible, false, true, true, true, true, false));
    QVERIFY(!TerminalCursorAnimation::shouldAnimate(
        visible, true, false, true, true, true, false));
    QVERIFY(!TerminalCursorAnimation::shouldAnimate(
        visible, true, true, true, false, true, false));
    QVERIFY(!TerminalCursorAnimation::shouldAnimate(
        visible, true, true, true, true, false, false));
    QVERIFY(!TerminalCursorAnimation::shouldAnimate(
        visible, true, true, true, true, true, true));
    cursor.blink = false;
    QVERIFY(!TerminalCursorAnimation::shouldAnimate(
        cursor, true, true, true, true, true, false));
    QVERIFY(!TerminalCursorAnimation::shouldAnimate(
        std::nullopt, true, true, true, true, true, false));
}

QTEST_MAIN(TerminalCursorAnimationTest)

#include "terminal_cursor_animation_test.moc"
