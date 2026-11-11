#pragma once

#include "terminal/api/terminal_types.h"

#include <QtGlobal>

#include <optional>

namespace span::terminal {

class TerminalCursorAnimation final {
public:
    static qreal opacityAt(qint64 elapsedMs);
    static bool presentationChanged(const Frame &before, const Frame &after);
    static bool shouldReset(const Frame &before, const Frame &after);
    static bool shouldAnimate(const std::optional<Cursor> &cursor,
                              bool liveViewport,
                              bool sessionRunning,
                              bool itemVisible,
                              bool windowActive,
                              bool focused,
                              bool compositionActive);
    static bool withinHold(qint64 elapsedMs);
    static bool shouldRunFadeTimer(bool cursorBlinkActive,
                                   bool contentBusy,
                                   bool withinHold);
};

} // namespace span::terminal
