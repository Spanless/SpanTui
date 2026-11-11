#include "terminal/shared/terminal_frame_store.h"

#include <QMutexLocker>
#include <utility>

namespace span::terminal {
bool TerminalFrameStore::publish(TerminalFrameSnapshot snapshot) {
    // Always keep the freshest immutable frame. Grok's welcome logo shimmer
    // (and other ~12fps truecolor redraws) only change cell colors; dropping
    // intermediate snapshots while the GUI is busy freezes the animation on
    // the first painted frame.
    std::shared_ptr<const TerminalFrameSnapshot> previous;
    {
        QMutexLocker locker(&m_mutex);
        if (m_notificationPending) {
            previous = m_latest;
        }
    }
    // The GUI has not acknowledged m_latest, so this snapshot may be consumed
    // directly after an older frame: a full refresh it skips carries forward.
    // Rows need nothing; the view matches them by id and revision.
    if (previous && previous->fullRefresh) {
        snapshot.fullRefresh = true;
    }
    const auto latest =
        std::make_shared<const TerminalFrameSnapshot>(std::move(snapshot));
    QMutexLocker locker(&m_mutex);
    m_latest = latest;
    if (m_notificationPending) {
        return false;
    }
    m_notificationPending = true;
    m_notifiedSequence = m_latest->sequence;
    return true;
}

TerminalFrameSnapshot TerminalFrameStore::latest() const {
    const std::shared_ptr<const TerminalFrameSnapshot> snapshot = latestShared();
    return snapshot ? *snapshot : TerminalFrameSnapshot{};
}

std::shared_ptr<const TerminalFrameSnapshot> TerminalFrameStore::latestShared() const {
    QMutexLocker locker(&m_mutex);
    return m_latest;
}

FrameNotificationDisposition TerminalFrameStore::acknowledgeNotification(
    quint64 consumedSequence) {
    QMutexLocker locker(&m_mutex);
    if (!m_notificationPending || consumedSequence == 0) {
        return FrameNotificationDisposition::Rejected;
    }
    // The GUI may ACK either the sequence that opened the notification or a
    // later coalesced sequence it re-read from latest() before painting.
    if (consumedSequence < m_notifiedSequence || consumedSequence > m_latest->sequence) {
        return FrameNotificationDisposition::Rejected;
    }
    if (consumedSequence < m_latest->sequence) {
        m_notifiedSequence = m_latest->sequence;
        return FrameNotificationDisposition::NotifyAgain;
    }
    m_notificationPending = false;
    m_notifiedSequence = 0;
    return FrameNotificationDisposition::Released;
}

void TerminalFrameStore::discardNotification() {
    QMutexLocker locker(&m_mutex);
    m_notificationPending = false;
    m_notifiedSequence = 0;
}

bool TerminalFrameStore::notificationPending() const {
    QMutexLocker locker(&m_mutex);
    return m_notificationPending;
}

} // namespace span::terminal
