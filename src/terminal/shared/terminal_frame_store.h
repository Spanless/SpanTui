#pragma once

#include "terminal/api/terminal_types.h"

#include <QMutex>
#include <memory>

namespace span::terminal {
enum class FrameNotificationDisposition : quint8 {
    Rejected,
    Released,
    NotifyAgain,
};

class TerminalFrameStore final {
public:
    // True when the GUI needs a notification; false when one is outstanding
    // (latest() still holds the newer frame).
    bool publish(TerminalFrameSnapshot snapshot);
    TerminalFrameSnapshot latest() const;
    std::shared_ptr<const TerminalFrameSnapshot> latestShared() const;
    // When a snapshot newer than the copied one exists, the reservation is
    // re-armed atomically: exactly one follow-up notification.
    FrameNotificationDisposition acknowledgeNotification(quint64 consumedSequence);
    // Used by the producer thread when a render consumer detaches. Serialized
    // with publish() so a hide/show boundary cannot retain a stale reservation.
    void discardNotification();
    bool notificationPending() const;

private:
    mutable QMutex m_mutex;
    std::shared_ptr<const TerminalFrameSnapshot> m_latest =
        std::make_shared<const TerminalFrameSnapshot>();
    bool m_notificationPending = false;
    // Sequence that opened the current notification. Coalesced publishes may
    // advance m_latest.sequence while this stays put until the GUI ACKs.
    quint64 m_notifiedSequence = 0;
};

} // namespace span::terminal
