// Throughput benchmark of the terminal core worker: a producer streams
// synchronized updates for ten seconds while a 60 Hz consumer acknowledges
// frames. It prints update/snapshot counts and CPU time, and fails only on
// gross regressions. Built with the tests but not registered in CTest — run
// it by hand to compare the parse/publish path before and after a change:
//   <build>\terminal_core_worker_benchmark.exe
#include "terminal/core/terminal_core_worker.h"
#include "terminal/shared/terminal_frame_store.h"
#include "terminal/shared/terminal_output_queue.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QScopeGuard>
#include <QTest>
#include <QThread>

#include <atomic>
#include <chrono>
#include <thread>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

using namespace span::terminal;

namespace {

QString frameText(const Frame &frame) {
    QString result;
    for (const Line &line : frame.lines) {
        for (const Cell &cell : line.cells) {
            if (!cell.continuation) {
                result += cell.text.isEmpty() ? QStringLiteral(" ") : cell.text;
            }
        }
        result += QLatin1Char('\n');
    }
    return result;
}

double processCpuSeconds() {
    FILETIME creation{}, exit{}, kernel{}, user{};
    if (!GetProcessTimes(GetCurrentProcess(), &creation, &exit, &kernel, &user)) {
        return 0.0;
    }
    ULARGE_INTEGER kernelTicks{};
    kernelTicks.LowPart = kernel.dwLowDateTime;
    kernelTicks.HighPart = kernel.dwHighDateTime;
    ULARGE_INTEGER userTicks{};
    userTicks.LowPart = user.dwLowDateTime;
    userTicks.HighPart = user.dwHighDateTime;
    return double(kernelTicks.QuadPart + userTicks.QuadPart) / 10000000.0;
}

void stopWorker(QThread *thread, TerminalCoreWorker *worker) {
    if (thread->isRunning()) {
        QThread *ownerThread = QThread::currentThread();
        QMetaObject::invokeMethod(worker, [worker, ownerThread]() {
            worker->shutdown();
            worker->moveToThread(ownerThread);
        }, Qt::BlockingQueuedConnection);
        thread->quit();
        thread->wait(30000);
    }
    delete worker;
}

void attachQueueWake(const std::shared_ptr<TerminalOutputQueue> &queue,
                     TerminalCoreWorker *worker) {
    queue->setWakeCallback([worker]() {
        QMetaObject::invokeMethod(worker,
                                  &TerminalCoreWorker::drainOutput,
                                  Qt::QueuedConnection);
    });
}

} // namespace

class TerminalCoreWorkerBenchmark final : public QObject {
    Q_OBJECT

private slots:
    void sustainsTenSecondSynchronizedUpdateStream();
};

void TerminalCoreWorkerBenchmark::sustainsTenSecondSynchronizedUpdateStream() {
    auto queue = std::make_shared<TerminalOutputQueue>();
    auto store = std::make_shared<TerminalFrameStore>();
    queue->open();

    QThread thread;
    auto *worker = new TerminalCoreWorker(
        QStringLiteral("worker-sustained-test-%1").arg(QCoreApplication::applicationPid()),
        80, 8, 128, queue, store);
    worker->moveToThread(&thread);
    thread.start();
    attachQueueWake(queue, worker);
    const auto workerGuard = qScopeGuard([&]() {
        queue->clearWakeCallback();
        stopWorker(&thread, worker);
    });
    QVERIFY(QMetaObject::invokeMethod(worker, &TerminalCoreWorker::initialize,
                                      Qt::BlockingQueuedConnection));
    QMetaObject::invokeMethod(worker, [worker]() {
        worker->setRenderConsumer(true, 60);
    }, Qt::QueuedConnection);
    QTRY_VERIFY_WITH_TIMEOUT(store->latest().sequence > 0, 5000);

    auto acknowledge = [&]() {
        const quint64 sequence = store->latest().sequence;
        if (sequence != 0) {
            store->acknowledgeNotification(sequence);
        }
    };
    acknowledge();

    std::atomic_bool stopProducer{false};
    std::atomic_int produced{0};
    std::thread producer([&]() {
        while (!stopProducer.load()) {
            const int number = produced.fetch_add(1);
            const QByteArray update = QByteArrayLiteral("\x1b[?2026h\x1b[Hsync-") +
                                      QByteArray::number(number) +
                                      QByteArrayLiteral("\x1b[K\x1b[?2026l");
            if (!queue->append(update)) {
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
    });
    const auto producerGuard = qScopeGuard([&]() {
        stopProducer.store(true);
        queue->close();
        if (producer.joinable()) {
            producer.join();
        }
    });

    const double cpuStart = processCpuSeconds();
    QElapsedTimer wall;
    wall.start();
    quint64 lastSequence = store->latest().sequence;
    int published = 0;
    while (wall.elapsed() < 10000) {
        const quint64 sequence = store->latest().sequence;
        if (sequence > lastSequence) {
            ++published;
            lastSequence = sequence;
            acknowledge();
        }
        QTest::qWait(1);
    }
    stopProducer.store(true);
    producer.join();
    QTRY_COMPARE_WITH_TIMEOUT(queue->size(), qsizetype(0), 10000);
    if (store->notificationPending()) {
        ++published;
        acknowledge();
    }

    const double cpuSeconds = processCpuSeconds() - cpuStart;
    qInfo().noquote()
        << QStringLiteral("terminal benchmark: updates=%1 snapshots=%2 cpu_seconds=%3 wall_ms=%4")
               .arg(produced.load())
               .arg(published)
               .arg(cpuSeconds, 0, 'f', 3)
               .arg(wall.elapsed());
    QVERIFY(produced.load() >= 500);
    QVERIFY(published >= 100);
    QVERIFY2(published <= 610,
             qPrintable(QStringLiteral("60 Hz worker published %1 frames in 10 seconds")
                            .arg(published)));
    QVERIFY(frameText(store->latest().frame).contains(QStringLiteral("sync-")));

    queue->close();
}

QTEST_MAIN(TerminalCoreWorkerBenchmark)

#include "terminal_core_worker_benchmark.moc"
