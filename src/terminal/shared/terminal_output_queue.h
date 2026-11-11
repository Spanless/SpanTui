#pragma once

#include <QByteArray>
#include <QMutex>
#include <QWaitCondition>

#include <deque>
#include <functional>

namespace span::terminal {
// Bounded PTY byte stream that keeps the producer's write boundaries. A
// boundary marks where one producer write ended with no further bytes
// immediately available; the Core may only present parser state at such a
// cut point (see TerminalPresentationScheduler).
class TerminalOutputQueue final {
public:
    bool open();
    void close();
    // The callback must be non-blocking and must not call back into this
    // queue. It is serialized with clearWakeCallback(), which makes it safe
    // to revoke the worker pointer before that QObject is destroyed.
    void setWakeCallback(std::function<void()> wakeWorker);
    void clearWakeCallback();
    // `endsWrite` marks the end of `bytes` as a producer write boundary. A
    // producer that knows more bytes of the same write follow passes false.
    bool append(const QByteArray &bytes, bool endsWrite = true);
    // Never crosses a boundary; `endsWrite` reports whether the returned
    // bytes end exactly at one.
    bool take(QByteArray *bytes, qsizetype maximumBytes, bool *endsWrite = nullptr);
    bool finishWake();
    qsizetype size() const;

private:
    mutable QMutex m_mutex;
    QWaitCondition m_spaceAvailable;
    QByteArray m_pending;
    qsizetype m_pendingOffset = 0;
    // Stream positions (total bytes ever appended) of unconsumed boundaries.
    std::deque<quint64> m_writeEnds;
    quint64 m_appendedBytes = 0;
    quint64 m_takenBytes = 0;
    std::function<void()> m_wakeWorker;
    bool m_accepting = false;
    bool m_wakeScheduled = false;
};

} // namespace span::terminal
