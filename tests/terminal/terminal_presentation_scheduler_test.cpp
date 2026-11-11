#include "terminal/core/terminal_presentation_scheduler.h"
#include "terminal/terminal_constants.h"

#include <QTest>

using namespace span::terminal;
using Scheduler = TerminalPresentationScheduler;

namespace {

// Output keeps arriving every 10 ms in [from, to); the times at which the
// scheduler would have published.
QList<qint64> publishesDuringBurst(Scheduler &scheduler, qint64 from, qint64 to)
{
    QList<qint64> published;
    for (qint64 now = from; now < to; now += 10) {
        scheduler.outputConsumed(now, true);
        if (scheduler.evaluate(now, false).publish) published.append(now);
    }
    return published;
}

} // namespace

class TerminalPresentationSchedulerTest final : public QObject {
    Q_OBJECT

private slots:
    void publishesCompleteWritesOnFrameCadence();
    void holdsPartialWriteUntilItsEnd();
    void cutsWriteThatNeverEndsAfterBoundedHold();
    void synchronizedEndIsACutPointInsideAWrite();
    void releasesStalledSynchronizedUpdateOnlyWhenBytesStop();
    void releasesImplicitRedrawWhenDrainedAndQuiet();
    void capsImplicitRedrawHold();
    void settlesRepaintAfterResize();
    void immediateBypassesCadenceButNotTransactions();
};

void TerminalPresentationSchedulerTest::publishesCompleteWritesOnFrameCadence() {
    Scheduler scheduler;
    scheduler.setFrameIntervalMs(16);
    QVERIFY(!scheduler.evaluate(0, false).publish);

    scheduler.markDirty(0);
    scheduler.outputConsumed(0, true);
    QVERIFY(scheduler.evaluate(0, false).publish);
    scheduler.published(0);

    scheduler.markDirty(5);
    scheduler.outputConsumed(5, true);
    const Scheduler::Decision early = scheduler.evaluate(5, false);
    QVERIFY(!early.publish);
    QCOMPARE(early.recheckAtMs, qint64(16));
    QVERIFY(scheduler.evaluate(16, false).publish);
}

void TerminalPresentationSchedulerTest::holdsPartialWriteUntilItsEnd() {
    Scheduler scheduler;
    scheduler.setFrameIntervalMs(16);
    scheduler.published(0);

    // The PTY delivered the first part of one write; the parser holds half a
    // frame. Being due on the cadence is not enough.
    scheduler.markDirty(20);
    scheduler.outputConsumed(20, false);
    const Scheduler::Decision held = scheduler.evaluate(20, true);
    QVERIFY(!held.publish);
    QCOMPARE(held.recheckAtMs, qint64(20 + kMidWriteHoldMs));
    QVERIFY(!scheduler.evaluate(40, false).publish);

    scheduler.outputConsumed(41, true);
    const Scheduler::Decision complete = scheduler.evaluate(41, false);
    QVERIFY(complete.publish);
    QVERIFY(!complete.forcedCut);
}

void TerminalPresentationSchedulerTest::cutsWriteThatNeverEndsAfterBoundedHold() {
    Scheduler scheduler;
    scheduler.setFrameIntervalMs(16);
    scheduler.published(0);
    scheduler.markDirty(100);
    scheduler.outputConsumed(100, false);
    QVERIFY(!scheduler.evaluate(149, true).publish);
    const Scheduler::Decision cut = scheduler.evaluate(150, true);
    QVERIFY(cut.publish);
    QVERIFY(cut.forcedCut);
}

void TerminalPresentationSchedulerTest::synchronizedEndIsACutPointInsideAWrite() {
    Scheduler scheduler;
    scheduler.setFrameIntervalMs(16);
    scheduler.published(0);
    scheduler.outputConsumed(30, false);
    scheduler.synchronizedBegin(30);
    scheduler.markDirty(30);
    QVERIFY(!scheduler.evaluate(31, true).publish);
    QVERIFY(scheduler.deferred());

    // The write continues past 2026l, but the application declared the
    // frame complete there.
    scheduler.synchronizedEnd(32);
    QVERIFY(!scheduler.deferred());
    const Scheduler::Decision decision = scheduler.evaluate(32, true);
    QVERIFY(decision.publish);
    QVERIFY(!decision.forcedCut);
}

void TerminalPresentationSchedulerTest::releasesStalledSynchronizedUpdateOnlyWhenBytesStop() {
    Scheduler scheduler;
    scheduler.setFrameIntervalMs(16);
    scheduler.synchronizedBegin(0);
    scheduler.markDirty(0);
    scheduler.outputConsumed(0, true);
    QCOMPARE(scheduler.evaluate(10, false).recheckAtMs, qint64(kSynchronizedStallMs));

    // A large frame still arriving keeps the transaction alive.
    scheduler.outputConsumed(200, true);
    QVERIFY(!scheduler.evaluate(400, false).publish);
    QVERIFY(scheduler.synchronizedActive());

    const Scheduler::Decision released = scheduler.evaluate(450, false);
    QCOMPARE(released.released, Scheduler::Release::SynchronizedStall);
    QVERIFY(released.publish);
    QVERIFY(!scheduler.synchronizedActive());

    // The late reset only completes recovery; a new set defers again.
    scheduler.published(450);
    scheduler.synchronizedEnd(460);
    QVERIFY(!scheduler.deferred());
    scheduler.synchronizedBegin(470);
    QVERIFY(scheduler.synchronizedActive());
}

void TerminalPresentationSchedulerTest::releasesImplicitRedrawWhenDrainedAndQuiet() {
    Scheduler scheduler;
    scheduler.setFrameIntervalMs(16);
    scheduler.redrawBegin(0);
    scheduler.markDirty(0);
    scheduler.outputConsumed(10, true);
    // A stray 2026 reset does not end the reprint.
    scheduler.synchronizedEnd(10);
    QVERIFY(scheduler.deferred());
    QVERIFY(!scheduler.evaluate(30, false).publish);
    // Queued bytes are the rest of the reprint.
    QVERIFY(!scheduler.evaluate(70, true).publish);

    const Scheduler::Decision released = scheduler.evaluate(70, false);
    QCOMPARE(released.released, Scheduler::Release::ImplicitRedraw);
    QVERIFY(released.publish);
}

void TerminalPresentationSchedulerTest::capsImplicitRedrawHold() {
    Scheduler scheduler;
    scheduler.redrawBegin(0);
    scheduler.markDirty(0);
    // A second ED3 keeps the original cap.
    scheduler.redrawBegin(500);
    QCOMPARE(publishesDuringBurst(scheduler, 0, kImplicitRedrawMaxHoldMs), QList<qint64>());
    QVERIFY(scheduler.evaluate(kImplicitRedrawMaxHoldMs, true).publish);
}

void TerminalPresentationSchedulerTest::settlesRepaintAfterResize() {
    Scheduler scheduler;
    scheduler.setFrameIntervalMs(16);
    scheduler.published(0);
    scheduler.repaintExpected(100);
    scheduler.markDirty(100);
    QCOMPARE(publishesDuringBurst(scheduler, 100, 200), QList<qint64>());
    QVERIFY(!scheduler.evaluate(239, false).publish);
    QVERIFY(scheduler.evaluate(240, false).publish);
    scheduler.published(240);

    // One continuous burst is presented after the bounded hold.
    scheduler.markDirty(300);
    QCOMPARE(publishesDuringBurst(scheduler, 300, 650), QList<qint64>());
    QVERIFY(scheduler.evaluate(650, false).publish);
    scheduler.published(650);

    // After the window, output is back on the cadence.
    scheduler.markDirty(900);
    scheduler.outputConsumed(900, true);
    QVERIFY(scheduler.evaluate(900, false).publish);
}

void TerminalPresentationSchedulerTest::immediateBypassesCadenceButNotTransactions() {
    Scheduler scheduler;
    scheduler.setFrameIntervalMs(100);
    scheduler.published(0);
    scheduler.markDirty(1);
    scheduler.requestImmediate();
    QVERIFY(scheduler.evaluate(1, false).publish);
    scheduler.published(1);

    scheduler.synchronizedBegin(2);
    scheduler.markDirty(2);
    scheduler.requestImmediate();
    QVERIFY(!scheduler.evaluate(3, false).publish);
    scheduler.synchronizedEnd(4);
    QVERIFY(scheduler.evaluate(4, false).publish);

    scheduler.redrawBegin(5);
    scheduler.resetProtocol();
    QVERIFY(!scheduler.deferred());
}

QTEST_GUILESS_MAIN(TerminalPresentationSchedulerTest)

#include "terminal_presentation_scheduler_test.moc"
