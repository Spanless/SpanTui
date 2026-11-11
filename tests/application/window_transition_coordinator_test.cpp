#include "application/window_transition_coordinator.h"

#include <QCoreApplication>
#include <QEventLoop>
#include <QQuickWindow>
#include <QSignalSpy>
#include <QTest>

using span::application::WindowTransitionCoordinator;

// Grows `window` one step at a time, from 900x600 to (899+steps)x(600+(steps-1)/2),
// pumping events between steps as a live drag does.
static void resizeStorm(QQuickWindow &window, int steps)
{
    for (int index = 0; index < steps; ++index) {
        window.resize(900 + index, 600 + index / 2);
        QCoreApplication::processEvents(QEventLoop::AllEvents, 1);
    }
}

class WindowTransitionCoordinatorTest final : public QObject {
    Q_OBJECT

private slots:
    void resizeStormCommitsOnlyLatestViewport();
    void localResizeHasExplicitPreviewSettleCommitPhases();
    void dpiAndStateChangesRestartSettlingWithoutExtraCommit();
    void nativeStateChangeDoesNotCommitStaleCanvasSize();
    void attachDoesNotCommitUntilSettle();
    void minimizeAndRestoreUsesRecoveryEpoch();
    void stalledNativeResizeIsForcedToCommit();
};

void WindowTransitionCoordinatorTest::resizeStormCommitsOnlyLatestViewport()
{
    QQuickWindow window;
    window.resize(900, 600);
    WindowTransitionCoordinator coordinator;
    coordinator.attachWindow(&window);
    const quint64 initialCommitRevision = coordinator.commitRevision();
    QSignalSpy previewed(&coordinator,
                         &WindowTransitionCoordinator::viewportPreviewed);
    QSignalSpy committed(&coordinator,
                         &WindowTransitionCoordinator::viewportCommitted);

    coordinator.beginNativeResize();
    resizeStorm(window, 1000);
    coordinator.endNativeResize();

    QVERIFY(previewed.count() > 1);
    QTRY_COMPARE_WITH_TIMEOUT(committed.count(), 1, 1000);
    QCOMPARE(committed.at(0).at(0).toInt(), 1899);
    QCOMPARE(committed.at(0).at(1).toInt(), 1099);
    QCOMPARE(coordinator.commitRevision(), initialCommitRevision + 1);
    QCOMPARE(coordinator.phase(), WindowTransitionCoordinator::Stable);
}

void WindowTransitionCoordinatorTest::localResizeHasExplicitPreviewSettleCommitPhases()
{
    QQuickWindow window;
    window.resize(1200, 720);
    WindowTransitionCoordinator coordinator;
    coordinator.attachWindow(&window);
    QSignalSpy committed(&coordinator,
                         &WindowTransitionCoordinator::viewportCommitted);

    coordinator.beginLocalResize();
    const quint64 epoch = coordinator.epoch();
    QCOMPARE(coordinator.phase(),
             WindowTransitionCoordinator::InteractiveResize);
    QVERIFY(coordinator.active());
    window.resize(1320, 760);
    QCoreApplication::processEvents();
    coordinator.endLocalResize();
    QCOMPARE(coordinator.phase(), WindowTransitionCoordinator::Settling);
    QTRY_COMPARE_WITH_TIMEOUT(committed.count(), 1, 1000);
    QCOMPARE(committed.at(0).at(0).toInt(), 1320);
    QCOMPARE(committed.at(0).at(1).toInt(), 760);
    QCOMPARE(committed.at(0).at(2).toULongLong(), epoch);
    QVERIFY(!coordinator.active());
}

void WindowTransitionCoordinatorTest::dpiAndStateChangesRestartSettlingWithoutExtraCommit()
{
    QQuickWindow window;
    window.resize(1024, 640);
    WindowTransitionCoordinator coordinator;
    coordinator.attachWindow(&window);
    QSignalSpy committed(&coordinator,
                         &WindowTransitionCoordinator::viewportCommitted);

    coordinator.noteNativeStateChange(2);
    const quint64 epoch = coordinator.epoch();
    coordinator.noteDpiChange();
    QCOMPARE(coordinator.epoch(), epoch);
    QCOMPARE(coordinator.phase(), WindowTransitionCoordinator::Settling);
    QTRY_COMPARE_WITH_TIMEOUT(committed.count(), 1, 1000);
}

void WindowTransitionCoordinatorTest::nativeStateChangeDoesNotCommitStaleCanvasSize()
{
    QQuickWindow window;
    window.resize(800, 600);
    WindowTransitionCoordinator coordinator;
    coordinator.attachWindow(&window);
    coordinator.previewLocalViewport(800, 552);
    QTRY_COMPARE_WITH_TIMEOUT(coordinator.phase(),
                              WindowTransitionCoordinator::Stable, 1000);
    QSignalSpy committed(&coordinator,
                         &WindowTransitionCoordinator::viewportCommitted);

    coordinator.noteNativeStateChange(2);
    window.resize(1600, 1000);
    QCoreApplication::processEvents();
    coordinator.previewLocalViewport(1600, 952);
    QTRY_COMPARE_WITH_TIMEOUT(committed.count(), 1, 1000);
    QCOMPARE(committed.at(0).at(0).toInt(), 1600);
    QCOMPARE(committed.at(0).at(1).toInt(), 952);
}


void WindowTransitionCoordinatorTest::attachDoesNotCommitUntilSettle()
{
    QQuickWindow window;
    window.resize(1320, 820);
    WindowTransitionCoordinator coordinator;
    QSignalSpy committed(&coordinator,
                         &WindowTransitionCoordinator::viewportCommitted);
    coordinator.attachWindow(&window);
    QCOMPARE(committed.count(), 0);
    QVERIFY(coordinator.active());
    QTRY_COMPARE_WITH_TIMEOUT(committed.count(), 1, 1000);
    QCOMPARE(coordinator.phase(), WindowTransitionCoordinator::Stable);
}

void WindowTransitionCoordinatorTest::minimizeAndRestoreUsesRecoveryEpoch()
{
    QQuickWindow window;
    window.resize(1024, 640);
    WindowTransitionCoordinator coordinator;
    coordinator.attachWindow(&window);
    const quint64 initialEpoch = coordinator.epoch();

    coordinator.beginNativeResize();
    QVERIFY(coordinator.epoch() > initialEpoch);
    const quint64 resizeEpoch = coordinator.epoch();
    coordinator.noteNativeStateChange(1);
    QCOMPARE(coordinator.phase(), WindowTransitionCoordinator::Minimized);
    QVERIFY(coordinator.epoch() > resizeEpoch);
    const quint64 minimizedEpoch = coordinator.epoch();
    coordinator.noteNativeStateChange(1);
    QCOMPARE(coordinator.epoch(), minimizedEpoch);

    coordinator.noteNativeStateChange(0);
    QCOMPARE(coordinator.phase(), WindowTransitionCoordinator::Recovering);
    QVERIFY(coordinator.epoch() > minimizedEpoch);
    const quint64 recoveringEpoch = coordinator.epoch();
    coordinator.noteDpiChange();
    QCOMPARE(coordinator.phase(), WindowTransitionCoordinator::Recovering);
    QCOMPARE(coordinator.epoch(), recoveringEpoch);
    QTRY_COMPARE_WITH_TIMEOUT(coordinator.phase(),
                              WindowTransitionCoordinator::Stable, 1000);
}

void WindowTransitionCoordinatorTest::stalledNativeResizeIsForcedToCommit()
{
    QQuickWindow window;
    window.resize(1000, 700);
    WindowTransitionCoordinator coordinator;
    coordinator.setStallWatchdogIntervalForTesting(80);
    coordinator.attachWindow(&window);
    QTRY_COMPARE_WITH_TIMEOUT(coordinator.phase(),
                              WindowTransitionCoordinator::Stable, 1000);
    QSignalSpy committed(&coordinator,
                         &WindowTransitionCoordinator::viewportCommitted);

    coordinator.beginNativeResize();
    window.resize(1100, 780);
    QCoreApplication::processEvents();
    QVERIFY(coordinator.active());
    // No endNativeResize arrives: the stall watchdog must force the commit so
    // effects can never stay frozen forever.
    QTRY_COMPARE_WITH_TIMEOUT(committed.count(), 1, 3000);
    QVERIFY(!coordinator.active());
    QCOMPARE(coordinator.phase(), WindowTransitionCoordinator::Stable);
    QCOMPARE(committed.at(0).at(0).toInt(), 1100);
    QCOMPARE(committed.at(0).at(1).toInt(), 780);
}

QTEST_MAIN(WindowTransitionCoordinatorTest)
#include "window_transition_coordinator_test.moc"
