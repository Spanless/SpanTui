#pragma once

#include "terminal/api/terminal_types.h"

#include <optional>

namespace span::terminal {

enum class TerminalImeAnchorSource : quint8 {
    None,
    ProtocolCursor,
    ReverseVideoCell,
};

struct TerminalImeAnchor {
    Cursor cursor;
    TerminalImeAnchorSource source = TerminalImeAnchorSource::None;

    bool isValid() const { return source != TerminalImeAnchorSource::None; }
};

struct TerminalCursorResolution {
    std::optional<Cursor> visualCursor;
    TerminalImeAnchor imeAnchor;
};

// Resolves painting and IME from one committed, same-epoch presentation. A
// hidden protocol cursor may still yield an IME fallback, but is never painted
// as SpanTui's own cursor. Nothing resolves unless the view shows the whole
// live screen (`liveViewport`), so a scrolled view keeps no cursor or anchor.
class TerminalCursorResolver final {
public:
    static TerminalCursorResolution resolve(const Frame &frame, quint64 frameTransportEpoch, bool liveViewport);
};

} // namespace span::terminal
