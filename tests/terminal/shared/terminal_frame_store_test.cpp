#include "terminal/shared/terminal_frame_store.h"

#include <QTest>

using namespace span::terminal;

class TerminalFrameStoreTest final : public QObject {
    Q_OBJECT

private slots:
    void coalescesSnapshotsBehindOneNotification();
    void carriesSkippedFullRefreshForward();
};

void TerminalFrameStoreTest::coalescesSnapshotsBehindOneNotification() {
    TerminalFrameStore store;
    TerminalFrameSnapshot first;
    first.sequence = 1;
    first.fullRefresh = false;
    TerminalFrameSnapshot second;
    second.sequence = 2;
    second.fullRefresh = false;
    QVERIFY(store.publish(first));
    const auto firstShared = store.latestShared();
    QVERIFY(firstShared);
    // A second publish while the GUI has not ACKed still updates latest so
    // color-only animations coalesce instead of freezing on the first frame.
    QVERIFY(!store.publish(second));
    const auto secondShared = store.latestShared();
    QVERIFY(secondShared);
    QVERIFY(secondShared != firstShared);
    QCOMPARE(firstShared->sequence, quint64(1));
    QCOMPARE(secondShared->sequence, quint64(2));
    QVERIFY(!secondShared->fullRefresh);
    // Consuming the original frame re-arms the one reservation because a newer
    // coalesced frame still needs a follow-up GUI notification.
    QVERIFY(store.acknowledgeNotification(1) ==
            FrameNotificationDisposition::NotifyAgain);
    QVERIFY(store.notificationPending());
    QVERIFY(store.acknowledgeNotification(1) ==
            FrameNotificationDisposition::Rejected);
    QVERIFY(store.acknowledgeNotification(2) ==
            FrameNotificationDisposition::Released);
    QVERIFY(!store.notificationPending());

    TerminalFrameSnapshot third;
    third.sequence = 3;
    QVERIFY(store.publish(third));
    QCOMPARE(store.latest().sequence, quint64(3));
    // Consumer re-read latest after coalesce-style updates and ACKs 3.
    QVERIFY(store.acknowledgeNotification(3) ==
            FrameNotificationDisposition::Released);

    TerminalFrameSnapshot fourth;
    fourth.sequence = 4;
    QVERIFY(store.publish(fourth));
    store.discardNotification();
    QVERIFY(!store.notificationPending());
}

void TerminalFrameStoreTest::carriesSkippedFullRefreshForward()
{
    // The GUI may consume the second snapshot without ever seeing the first:
    // the full refresh the first asked for must not be lost.
    TerminalFrameStore store;
    TerminalFrameSnapshot refresh;
    refresh.sequence = 1;
    refresh.fullRefresh = true;
    TerminalFrameSnapshot incremental;
    incremental.sequence = 2;
    incremental.fullRefresh = false;

    QVERIFY(store.publish(refresh));
    QVERIFY(!store.publish(incremental));
    QVERIFY(store.latest().fullRefresh);

    // Once consumed, later snapshots stand on their own.
    QVERIFY(store.acknowledgeNotification(2) == FrameNotificationDisposition::Released);
    TerminalFrameSnapshot next;
    next.sequence = 3;
    next.fullRefresh = false;
    QVERIFY(store.publish(next));
    QVERIFY(!store.latest().fullRefresh);
}

QTEST_MAIN(TerminalFrameStoreTest)

#include "terminal_frame_store_test.moc"
