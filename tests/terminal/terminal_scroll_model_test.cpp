#include "terminal/render/terminal_scroll_model.h"
#include "terminal/terminal_constants.h"

#include <QTest>

#include <algorithm>

using namespace span::terminal;

namespace {

struct Animation {
    // The top after each 16 ms frame, at most 200 of them.
    QList<double> tops;
    // Every frame reported that it moved.
    bool everyFrameMoved = true;
    qint64 end = 0;
};

Animation animate(TerminalScrollModel &model, qint64 start)
{
    Animation animation;
    animation.end = start;
    while (model.animating() && animation.tops.size() < 200) {
        animation.end += 16;
        animation.everyFrameMoved = model.advance(animation.end) && animation.everyFrameMoved;
        animation.tops.append(model.top());
    }
    return animation;
}

// Distance covered by each frame, starting from `from`.
QList<double> steps(double from, const QList<double> &tops)
{
    QList<double> distances;
    for (double top : tops) {
        distances.append(from - top);
        from = top;
    }
    return distances;
}

void settle(TerminalScrollModel &model, qint64 from)
{
    for (qint64 now = from; model.animating(); now += 16) model.advance(now);
}

} // namespace

class TerminalScrollModelTest final : public QObject {
    Q_OBJECT

private slots:
    void followsTheLiveScreenAtTheBottom();
    void keepsTheTopLineWhileScrolledUp();
    void clampsToTheHistoryThatIsLeft();
    void easesTowardTheTargetAndSettles();
    void accumulatesWheelStepsIntoOneTarget();
    void stalledFrameResumesTheEase();
    void immediateMovesEndTheAnimation();
    void reducedMotionScrollsImmediately();
};

void TerminalScrollModelTest::followsTheLiveScreenAtTheBottom()
{
    TerminalScrollModel model;
    QVERIFY(model.atBottom());
    QVERIFY(model.setRange(0, 10));
    QCOMPARE(model.top(), 10.0);
    QCOMPARE(model.offset(), 0.0);
    // New output moves the bottom; a following view goes with it.
    QVERIFY(model.setRange(0, 25));
    QCOMPARE(model.top(), 25.0);
    QVERIFY(model.atBottom());
    QVERIFY(!model.setRange(0, 25));
}

void TerminalScrollModelTest::keepsTheTopLineWhileScrolledUp()
{
    TerminalScrollModel model;
    model.setRange(0, 100);
    QVERIFY(model.scrollTo(40.5));
    QVERIFY(!model.atBottom());
    QCOMPARE(model.offset(), 59.5);
    // Output appends below; the text being read stays put and the distance
    // to the live screen grows.
    QVERIFY(!model.setRange(0, 130));
    QCOMPARE(model.top(), 40.5);
    QCOMPARE(model.offset(), 89.5);
    QVERIFY(model.scrollToBottom());
    QVERIFY(model.atBottom());
    QCOMPARE(model.top(), 130.0);
}

void TerminalScrollModelTest::clampsToTheHistoryThatIsLeft()
{
    TerminalScrollModel model;
    model.setRange(0, 100);
    QVERIFY(model.scrollTo(-20));
    QCOMPARE(model.top(), 0.0);
    QVERIFY(!model.scrollTo(-1));
    // The rows on top were trimmed: the view moves to the oldest kept line.
    QVERIFY(model.setRange(30, 130));
    QCOMPARE(model.top(), 30.0);
    QVERIFY(model.scrollTo(500));
    QVERIFY(model.atBottom());
    QCOMPARE(model.top(), 130.0);
    // A range without history (the alternate screen) pins the view down.
    model.scrollTo(60);
    QVERIFY(model.setRange(140, 140));
    QVERIFY(model.atBottom());
    QCOMPARE(model.offset(), 0.0);
}

void TerminalScrollModelTest::easesTowardTheTargetAndSettles()
{
    TerminalScrollModel model;
    model.setRange(0, 100);
    QVERIFY(!model.animateBy(-10, 1000));
    QVERIFY(model.animating());
    QCOMPARE(model.top(), 100.0);

    const Animation animation = animate(model, 1000);
    QVERIFY(animation.everyFrameMoved);
    QVERIFY(!model.animating());
    QCOMPARE(model.top(), 90.0);
    // Most of the distance is covered within a few frames.
    QVERIFY(animation.tops.size() < 30);
    QVERIFY(std::all_of(animation.tops.cbegin(), animation.tops.cend(), [](double top) { return top >= 90.0; }));
    const QList<double> covered = steps(100.0, animation.tops);
    QVERIFY(std::all_of(covered.cbegin(), covered.cend(), [](double step) { return step > 0; }));
    // Ease-out: every frame covers less distance than the one before, up to
    // the final snap onto the target.
    QVERIFY(std::is_sorted(covered.cbegin(), covered.cend() - 1,
                           [](double later, double earlier) { return later > earlier + 1e-9; }));
    QVERIFY(!model.advance(animation.end + 16));
}

void TerminalScrollModelTest::accumulatesWheelStepsIntoOneTarget()
{
    TerminalScrollModel model;
    model.setRange(0, 100);
    model.animateBy(-3, 0);
    model.advance(16);
    // A second notch adds to the target, not to the current position.
    model.animateBy(-3, 20);
    settle(model, 32);
    QCOMPARE(model.top(), 94.0);

    // Scrolling down onto the live screen follows it again, even when
    // output arrives during the animation.
    model.animateBy(10, 1000);
    model.advance(1016);
    model.setRange(0, 104);
    settle(model, 1032);
    QVERIFY(model.atBottom());
    QCOMPARE(model.top(), 104.0);
}

void TerminalScrollModelTest::stalledFrameResumesTheEase()
{
    // The same animation once with a frame every 33 ms, once with a
    // second-long stall after the first frame.
    TerminalScrollModel steady;
    steady.setRange(0, 100);
    steady.animateBy(-40, 0);
    steady.advance(16);
    steady.advance(16 + kScrollMaxFrameMs);

    TerminalScrollModel stalled;
    stalled.setRange(0, 100);
    stalled.animateBy(-40, 0);
    stalled.advance(16);
    QVERIFY(stalled.advance(1016));
    // The frame after the stall moves as far as one long frame, not onto the
    // target, and the ease goes on from there.
    QVERIFY(stalled.animating());
    QCOMPARE(stalled.top(), steady.top());
    QVERIFY(stalled.top() > 70.0);
    settle(stalled, 1032);
    QCOMPARE(stalled.top(), 60.0);
}

void TerminalScrollModelTest::immediateMovesEndTheAnimation()
{
    TerminalScrollModel model;
    model.setRange(0, 100);
    model.animateBy(-20, 0);
    model.advance(16);
    QVERIFY(model.scrollTo(50));
    QVERIFY(!model.animating());
    QCOMPARE(model.top(), 50.0);
    model.animateBy(-5, 100);
    model.reset();
    QVERIFY(!model.animating());
    QVERIFY(model.atBottom());
    QCOMPARE(model.top(), 0.0);
}

void TerminalScrollModelTest::reducedMotionScrollsImmediately()
{
    TerminalScrollModel model;
    model.setRange(0, 100);
    model.setAnimated(false);
    QVERIFY(model.animateBy(-7, 0));
    QVERIFY(!model.animating());
    QCOMPARE(model.top(), 93.0);

    // Turning animation off mid-way lands on the target.
    model.setAnimated(true);
    model.animateBy(-3, 100);
    model.advance(116);
    model.setAnimated(false);
    QVERIFY(!model.animating());
    QCOMPARE(model.top(), 90.0);
}

QTEST_GUILESS_MAIN(TerminalScrollModelTest)
#include "terminal_scroll_model_test.moc"
