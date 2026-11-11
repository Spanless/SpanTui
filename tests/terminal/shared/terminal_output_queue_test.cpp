#include "terminal/shared/terminal_output_queue.h"
#include "terminal/terminal_constants.h"

#include <QTest>

#include <algorithm>
#include <atomic>
#include <memory>
#include <thread>

using namespace span::terminal;

namespace {

// `count` 4 KiB chunks, each starting "marker-<n>:" and padded with a
// letter of its own.
QByteArray markedChunks(int count)
{
    QByteArray bytes;
    for (int index = 0; index < count; ++index) {
        QByteArray chunk = QByteArrayLiteral("marker-") + QByteArray::number(index) + QByteArrayLiteral(":");
        chunk.append(QByteArray(4096 - chunk.size(), char('a' + index % 26)));
        bytes.append(chunk);
    }
    return bytes;
}

void appendInPieces(TerminalOutputQueue &queue, const QByteArray &bytes, qsizetype piece)
{
    for (qsizetype offset = 0; offset < bytes.size(); offset += piece) queue.append(bytes.mid(offset, piece));
}

// Takes until the producer is done and the queue is empty.
QByteArray drainWhileProducing(TerminalOutputQueue &queue, const std::atomic_bool &producerDone, qsizetype maxBytes)
{
    QByteArray received;
    while (!producerDone.load() || queue.size() > 0) {
        QByteArray chunk;
        if (queue.take(&chunk, maxBytes))
            received.append(chunk);
        else
            QTest::qWait(1);
    }
    return received;
}

QByteArray drainAvailable(TerminalOutputQueue &queue, qsizetype maxBytes)
{
    QByteArray received;
    QByteArray chunk;
    while (queue.take(&chunk, maxBytes)) received.append(chunk);
    return received;
}

struct Drained {
    QByteArray bytes;
    // Received sizes after each take that ended a producer write.
    QList<qsizetype> writeEnds;
};

// Takes until `size` bytes arrived, waiting while a producer refills.
Drained drainUntil(TerminalOutputQueue &queue, qsizetype size, qsizetype maxBytes)
{
    Drained drained;
    while (drained.bytes.size() < size) {
        QByteArray chunk;
        bool endsWrite = false;
        if (!queue.take(&chunk, maxBytes, &endsWrite)) {
            QTest::qWait(1);
            continue;
        }
        drained.bytes += chunk;
        if (endsWrite) drained.writeEnds.append(drained.bytes.size());
    }
    return drained;
}

void spinUntil(const std::atomic_bool &flag)
{
    while (!flag.load()) std::this_thread::yield();
}

} // namespace

class TerminalOutputQueueTest final : public QObject {
    Q_OBJECT

private slots:
    void preservesAllBytesUnderBackpressure();
    void preservesOrderAcrossDeferredCompaction();
    void serializesWakeCallbackRevocation();
    void reportsProducerWriteEnds();
};

void TerminalOutputQueueTest::preservesAllBytesUnderBackpressure() {
    auto queue = std::make_shared<TerminalOutputQueue>();
    QVERIFY(queue->open() || queue->size() == 0);

    const QByteArray expected = markedChunks(512);
    std::atomic_bool producerDone{false};
    std::thread producer([&]() {
        appendInPieces(*queue, expected, 4096);
        producerDone.store(true);
    });
    const QByteArray received = drainWhileProducing(*queue, producerDone, 8192);
    producer.join();
    queue->close();
    QCOMPARE(received, expected);
}

void TerminalOutputQueueTest::preservesOrderAcrossDeferredCompaction() {
    TerminalOutputQueue queue;
    QVERIFY(queue.open() || queue.size() == 0);

    const QByteArray first(96 * 1024, 'a');
    const QByteArray second(48 * 1024, 'b');
    QVERIFY(queue.append(first));

    QByteArray received;
    QByteArray chunk;
    QVERIFY(queue.take(&chunk, 32 * 1024));
    received.append(chunk);
    QCOMPARE(queue.size(), qsizetype(64 * 1024));

    QVERIFY(queue.append(second));
    QCOMPARE(queue.size(), qsizetype(112 * 1024));
    received.append(drainAvailable(queue, 7 * 1024));

    QCOMPARE(received, first + second);
    QCOMPARE(queue.size(), qsizetype(0));
    QVERIFY(!queue.finishWake());
    queue.close();
}

void TerminalOutputQueueTest::serializesWakeCallbackRevocation() {
    TerminalOutputQueue queue;
    queue.open();
    std::atomic_bool entered{false};
    std::atomic_bool release{false};
    std::atomic_bool cleared{false};
    queue.setWakeCallback([&]() {
        entered.store(true);
        spinUntil(release);
    });

    std::thread producer([&]() { queue.append(QByteArrayLiteral("wake")); });
    QTRY_VERIFY_WITH_TIMEOUT(entered.load(), 2000);
    std::thread clearer([&]() {
        queue.clearWakeCallback();
        cleared.store(true);
    });
    QTest::qWait(20);
    const bool clearWaitedForCallback = !cleared.load();
    release.store(true);
    producer.join();
    clearer.join();
    QVERIFY(clearWaitedForCallback);

    QByteArray bytes;
    QVERIFY(queue.take(&bytes, 16));
    QCOMPARE(bytes, QByteArrayLiteral("wake"));
    queue.finishWake();
    queue.close();
}

void TerminalOutputQueueTest::reportsProducerWriteEnds() {
    TerminalOutputQueue queue;
    QVERIFY(queue.open() || queue.size() == 0);

    // One producer write delivered in two reads, then a complete write.
    QVERIFY(queue.append(QByteArrayLiteral("frame-a1"), false));
    QVERIFY(queue.append(QByteArrayLiteral("frame-a2")));
    QVERIFY(queue.append(QByteArrayLiteral("frame-b")));

    QByteArray chunk;
    bool endsWrite = true;
    QVERIFY(queue.take(&chunk, 4, &endsWrite));
    QCOMPARE(chunk, QByteArrayLiteral("fram"));
    QVERIFY(!endsWrite);
    // A take never crosses a write end, even with room for more.
    QVERIFY(queue.take(&chunk, 1024, &endsWrite));
    QCOMPARE(chunk, QByteArrayLiteral("e-a1frame-a2"));
    QVERIFY(endsWrite);
    QVERIFY(queue.take(&chunk, 1024, &endsWrite));
    QCOMPARE(chunk, QByteArrayLiteral("frame-b"));
    QVERIFY(endsWrite);
    QVERIFY(!queue.take(&chunk, 1024, &endsWrite));

    // A write larger than the queue is split by backpressure; only its final
    // byte is a write end.
    const QByteArray large(kOutputQueueMaximumBytes + 4096, 'x');
    std::thread producer([&]() { queue.append(large); });
    const Drained drained = drainUntil(queue, large.size(), 64 * 1024);
    producer.join();
    QCOMPARE(drained.bytes, large);
    QCOMPARE(drained.writeEnds, QList<qsizetype>{large.size()});
    queue.close();
}

QTEST_MAIN(TerminalOutputQueueTest)

#include "terminal_output_queue_test.moc"
