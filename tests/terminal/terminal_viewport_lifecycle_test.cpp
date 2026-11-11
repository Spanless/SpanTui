#include "terminal/render/terminal_viewport_lifecycle.h"

#include <QTest>

using namespace span::terminal;

class TerminalViewportLifecycleTest final : public QObject {
    Q_OBJECT

private slots:
    void positionChangesHaveNoViewportCommit();
    void movingCoalescesToFinalStableSize();
    void pixelChangeWithinSameGridCommitsRenderOnly();
    void hiddenRestoreDoesNotCreateAViewportCommit();
    void closingRejectsFurtherViewportCommits();
};

void TerminalViewportLifecycleTest::positionChangesHaveNoViewportCommit()
{
    TerminalViewportLifecycle lifecycle;
    lifecycle.setCellSize(QSizeF(10, 20));
    lifecycle.setContentSize(QSizeF(120, 100));
    const auto first = lifecycle.takeStableCommit();
    QVERIFY(first);
    QCOMPARE(first.columns, 12);
    QCOMPARE(first.rows, 5);

    // Position is intentionally not an input to this object.
    QVERIFY(!lifecycle.takeStableCommit());
    lifecycle.setState(TerminalViewportLifecycle::State::Moving);
    lifecycle.setState(TerminalViewportLifecycle::State::Normal);
    QVERIFY(!lifecycle.takeStableCommit());
}

void TerminalViewportLifecycleTest::movingCoalescesToFinalStableSize()
{
    TerminalViewportLifecycle lifecycle;
    lifecycle.setCellSize(QSizeF(10, 20));
    lifecycle.setContentSize(QSizeF(120, 100));
    QVERIFY(lifecycle.takeStableCommit());

    lifecycle.setState(TerminalViewportLifecycle::State::Moving);
    lifecycle.setContentSize(QSizeF(140, 100));
    lifecycle.setContentSize(QSizeF(190, 100));
    QVERIFY(!lifecycle.takeStableCommit());

    lifecycle.setState(TerminalViewportLifecycle::State::Normal);
    const auto finalCommit = lifecycle.takeStableCommit();
    QVERIFY(finalCommit);
    QCOMPARE(finalCommit.columns, 19);
    QCOMPARE(finalCommit.rows, 5);
    QVERIFY(finalCommit.gridChanged);
    QVERIFY(!lifecycle.takeStableCommit());
}

void TerminalViewportLifecycleTest::pixelChangeWithinSameGridCommitsRenderOnly()
{
    TerminalViewportLifecycle lifecycle;
    lifecycle.setCellSize(QSizeF(10, 20));
    lifecycle.setContentSize(QSizeF(120, 100));
    QVERIFY(lifecycle.takeStableCommit());

    lifecycle.setState(TerminalViewportLifecycle::State::Moving);
    lifecycle.setContentSize(QSizeF(125, 105));
    lifecycle.setState(TerminalViewportLifecycle::State::Normal);

    const auto commit = lifecycle.takeStableCommit();
    QVERIFY(commit);
    QCOMPARE(commit.columns, 12);
    QCOMPARE(commit.rows, 5);
    QVERIFY(!commit.gridChanged);
}

void TerminalViewportLifecycleTest::hiddenRestoreDoesNotCreateAViewportCommit()
{
    TerminalViewportLifecycle lifecycle;
    lifecycle.setCellSize(QSizeF(10, 20));
    lifecycle.setContentSize(QSizeF(120, 100));
    QVERIFY(lifecycle.takeStableCommit());

    lifecycle.setState(TerminalViewportLifecycle::State::Hidden);
    QVERIFY(!lifecycle.takeStableCommit());
    lifecycle.setState(TerminalViewportLifecycle::State::Normal);
    // Render-consumer reactivation owns frame recovery. Presentation state
    // alone must not manufacture another viewport or frame request.
    QVERIFY(!lifecycle.takeStableCommit());
}

void TerminalViewportLifecycleTest::closingRejectsFurtherViewportCommits()
{
    TerminalViewportLifecycle lifecycle;
    lifecycle.setCellSize(QSizeF(10, 20));
    lifecycle.setContentSize(QSizeF(120, 100));
    QVERIFY(lifecycle.takeStableCommit());

    lifecycle.setState(TerminalViewportLifecycle::State::Closing);
    lifecycle.setContentSize(QSizeF(240, 100));
    QVERIFY(!lifecycle.takeStableCommit());
}

QTEST_MAIN(TerminalViewportLifecycleTest)

#include "terminal_viewport_lifecycle_test.moc"
