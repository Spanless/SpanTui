#include "terminal/shared/terminal_output_queue.h"

#include "terminal/terminal_constants.h"

#include <QMutexLocker>
#include <algorithm>
#include <utility>

namespace span::terminal {
bool TerminalOutputQueue::open() {
    QMutexLocker locker(&m_mutex);
    m_accepting = true;
    if (m_pendingOffset < m_pending.size() && !m_wakeScheduled) {
        m_wakeScheduled = true;
        return true;
    }
    return false;
}

void TerminalOutputQueue::close() {
    QMutexLocker locker(&m_mutex);
    m_accepting = false;
    m_spaceAvailable.wakeAll();
}

void TerminalOutputQueue::setWakeCallback(std::function<void()> wakeWorker) {
    QMutexLocker locker(&m_mutex);
    m_wakeWorker = std::move(wakeWorker);
}

void TerminalOutputQueue::clearWakeCallback() {
    QMutexLocker locker(&m_mutex);
    m_wakeWorker = {};
}

bool TerminalOutputQueue::append(const QByteArray &bytes, bool endsWrite) {
    if (bytes.isEmpty()) {
        return true;
    }

    qsizetype offset = 0;
    while (offset < bytes.size()) {
        QMutexLocker locker(&m_mutex);
        qsizetype pendingBytes = m_pending.size() - m_pendingOffset;
        while (m_accepting && pendingBytes >= kOutputQueueMaximumBytes) {
            m_spaceAvailable.wait(&m_mutex);
            pendingBytes = m_pending.size() - m_pendingOffset;
        }
        if (!m_accepting) {
            return false;
        }
        const qsizetype available = kOutputQueueMaximumBytes - pendingBytes;
        const qsizetype count = std::min(available, bytes.size() - offset);
        m_pending.append(bytes.constData() + offset, count);
        offset += count;
        m_appendedBytes += quint64(count);
        // Recorded under the same lock as the final bytes, so a consumer
        // never takes them without seeing the boundary.
        if (endsWrite && offset == bytes.size()) m_writeEnds.push_back(m_appendedBytes);
        if (!m_wakeScheduled) {
            m_wakeScheduled = true;
            if (m_wakeWorker) {
                m_wakeWorker();
            }
        }
    }
    return true;
}

bool TerminalOutputQueue::take(QByteArray *bytes, qsizetype maximumBytes, bool *endsWrite) {
    if (!bytes || maximumBytes <= 0) {
        return false;
    }
    QMutexLocker locker(&m_mutex);
    const qsizetype pendingBytes = m_pending.size() - m_pendingOffset;
    if (pendingBytes <= 0) {
        return false;
    }
    qsizetype count = std::min(maximumBytes, pendingBytes);
    if (!m_writeEnds.empty()) {
        count = qsizetype(std::min<quint64>(quint64(count), m_writeEnds.front() - m_takenBytes));
    }
    *bytes = QByteArray(m_pending.constData() + m_pendingOffset, count);
    m_pendingOffset += count;
    m_takenBytes += quint64(count);
    const bool atWriteEnd = !m_writeEnds.empty() && m_writeEnds.front() == m_takenBytes;
    if (atWriteEnd) m_writeEnds.pop_front();
    if (endsWrite) *endsWrite = atWriteEnd;
    if (m_pendingOffset == m_pending.size()) {
        m_pending.clear();
        m_pendingOffset = 0;
    } else if (m_pendingOffset >= 64 * 1024
               && m_pendingOffset >= m_pending.size() / 2) {
        m_pending.remove(0, m_pendingOffset);
        m_pendingOffset = 0;
    }
    m_spaceAvailable.wakeAll();
    return true;
}

bool TerminalOutputQueue::finishWake() {
    QMutexLocker locker(&m_mutex);
    m_wakeScheduled = false;
    if (m_pendingOffset < m_pending.size()) {
        m_wakeScheduled = true;
        return true;
    }
    return false;
}

qsizetype TerminalOutputQueue::size() const {
    QMutexLocker locker(&m_mutex);
    return m_pending.size() - m_pendingOffset;
}

} // namespace span::terminal
