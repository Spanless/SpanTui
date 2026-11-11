#pragma once

#include <QMetaType>
#include <QtGlobal>

#include <atomic>
#include <memory>
#include <mutex>

namespace span::terminal {

enum class TerminalInputKind : quint8 { User, Protocol, PasteBegin, PasteEnd };

// Transport lifetime and user cancellation are independent: interrupt must not
// revoke protocol replies, while replacing a child revokes every old packet.
struct TerminalInputEpoch {
    std::mutex outputMutex;
    std::atomic_bool active{true};
    std::atomic<quint64> userSerial{1};
};

struct TerminalInputToken {
    std::shared_ptr<TerminalInputEpoch> epoch;
    quint64 userSerial = 0;
    TerminalInputKind kind = TerminalInputKind::Protocol;

    bool valid() const {
        return epoch && epoch->active.load(std::memory_order_acquire)
            && (kind == TerminalInputKind::Protocol
                || userSerial == epoch->userSerial.load(std::memory_order_acquire));
    }
};

} // namespace span::terminal

Q_DECLARE_METATYPE(span::terminal::TerminalInputToken)
