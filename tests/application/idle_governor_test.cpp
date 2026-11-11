#include "application/idle_governor.h"

#include <QGuiApplication>
#include <QKeyEvent>
#include <QSignalSpy>
#include <QTest>

using span::application::IdleGovernor;

class IdleGovernorTest final : public QObject {
    Q_OBJECT

private slots:
    void inputTimeoutMarksIdle();
    void activityResetsTimeout();
    void applicationInactivityMarksIdle();
    void deliveredInputEventCountsAsActivity();
    void noDuplicateEmissions();
    void timeoutChangedWhileIdleAppliesToNextActivity();
    void rejectsNonPositiveTimeout();
};

void IdleGovernorTest::inputTimeoutMarksIdle()
{
    IdleGovernor governor;
    governor.setInputTimeoutMs(50);
    QSignalSpy spy(&governor, &IdleGovernor::idleChanged);
    QCOMPARE(governor.idle(), false);
    QTRY_VERIFY_WITH_TIMEOUT(governor.idle(), 1000);
    QCOMPARE(spy.count(), 1);
}

void IdleGovernorTest::activityResetsTimeout()
{
    IdleGovernor governor;
    governor.setInputTimeoutMs(50);
    QSignalSpy spy(&governor, &IdleGovernor::idleChanged);
    QTRY_VERIFY_WITH_TIMEOUT(governor.idle(), 1000);
    QCOMPARE(spy.count(), 1);

    governor.noteActivity();
    QCOMPARE(governor.idle(), false);
    QCOMPARE(spy.count(), 2);
}

void IdleGovernorTest::applicationInactivityMarksIdle()
{
    IdleGovernor governor;
    governor.setInputTimeoutMs(50);
    QSignalSpy spy(&governor, &IdleGovernor::idleChanged);

    governor.setApplicationActive(false);
    QCOMPARE(governor.idle(), true);
    QCOMPARE(spy.count(), 1);

    governor.setApplicationActive(true);
    QCOMPARE(governor.idle(), false);
    QCOMPARE(spy.count(), 2);

    // Reactivation restarts the input clock: with no activity the governor
    // returns to idle once the timeout elapses again.
    QTRY_VERIFY_WITH_TIMEOUT(governor.idle(), 1000);
    QCOMPARE(spy.count(), 3);
}

void IdleGovernorTest::deliveredInputEventCountsAsActivity()
{
    IdleGovernor governor;
    governor.setInputTimeoutMs(50);
    governor.attach(qGuiApp);
    // The offscreen platform may report the application as inactive; the
    // input-path assertion below only concerns the timeout leg.
    governor.setApplicationActive(true);
    QSignalSpy spy(&governor, &IdleGovernor::idleChanged);
    QTRY_VERIFY_WITH_TIMEOUT(governor.idle(), 1000);
    QCOMPARE(spy.count(), 1);

    QObject target;
    QKeyEvent press(QEvent::KeyPress, Qt::Key_A, Qt::NoModifier);
    QCoreApplication::sendEvent(&target, &press);
    QCOMPARE(governor.idle(), false);
    QCOMPARE(spy.count(), 2);
}

void IdleGovernorTest::noDuplicateEmissions()
{
    IdleGovernor governor;
    governor.setInputTimeoutMs(50);
    QSignalSpy spy(&governor, &IdleGovernor::idleChanged);

    // Each change is reported once: repeated activity while active, the
    // timeout lapsing again while idle, and the inactive window on top of
    // the input timeout are all silent.
    governor.noteActivity();
    governor.noteActivity();
    QCOMPARE(governor.idle(), false);
    QCOMPARE(spy.count(), 0);

    QTRY_VERIFY_WITH_TIMEOUT(governor.idle(), 1000);
    QCOMPARE(spy.count(), 1);
    QTest::qWait(150);
    governor.setApplicationActive(false);
    QCOMPARE(spy.count(), 1);

    governor.setApplicationActive(true);
    governor.noteActivity();
    governor.noteActivity();
    QCOMPARE(governor.idle(), false);
    QCOMPARE(spy.count(), 2);
}

void IdleGovernorTest::timeoutChangedWhileIdleAppliesToNextActivity()
{
    IdleGovernor governor;
    governor.setInputTimeoutMs(50);
    QTRY_VERIFY_WITH_TIMEOUT(governor.idle(), 1000);

    // The countdown has already fired; the new timeout still governs the
    // next one instead of the 30 s default.
    governor.setInputTimeoutMs(60);
    governor.noteActivity();
    QCOMPARE(governor.idle(), false);
    QTRY_VERIFY_WITH_TIMEOUT(governor.idle(), 1000);
}

void IdleGovernorTest::rejectsNonPositiveTimeout()
{
    IdleGovernor governor;
    governor.setInputTimeoutMs(200);
    governor.setInputTimeoutMs(0);
    governor.setInputTimeoutMs(-5);
    // A zero interval would fire at once; the 200 ms timeout stays in force.
    QTest::qWait(20);
    QCOMPARE(governor.idle(), false);
    QTRY_VERIFY_WITH_TIMEOUT(governor.idle(), 1000);
}

QTEST_MAIN(IdleGovernorTest)
#include "idle_governor_test.moc"
