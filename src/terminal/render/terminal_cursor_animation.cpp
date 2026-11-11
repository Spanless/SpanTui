#include "terminal/render/terminal_cursor_animation.h"

#include "terminal/terminal_constants.h"

#include <QtMath>

#include <cmath>

namespace span::terminal {

qreal TerminalCursorAnimation::opacityAt(qint64 elapsedMs) {
    const qint64 normalized = ((elapsedMs % kCursorBlinkCycleMs) + kCursorBlinkCycleMs) % kCursorBlinkCycleMs;
    const qreal phase = qreal(normalized) / qreal(kCursorBlinkCycleMs);
    const qreal fade = (1.0 - std::cos(2.0 * M_PI * phase)) / 2.0;
    return 1.0 - (1.0 - kCursorMinimumOpacity) * fade;
}

bool TerminalCursorAnimation::presentationChanged(const Frame &before,
                                                   const Frame &after) {
    return before.cursor.row != after.cursor.row || before.cursor.column != after.cursor.column
        || before.cursor.visible != after.cursor.visible || before.cursor.blink != after.cursor.blink
        || before.cursor.shape != after.cursor.shape;
}

bool TerminalCursorAnimation::shouldReset(const Frame &before,
                                          const Frame &after) {
    // Position changes are common while a TUI paints a frame. Restarting at
    // full opacity for every move turns a smooth cycle into rapid flashes.
    // Keep the phase continuous and reset only when presentation semantics
    // change.
    return before.cursor.visible != after.cursor.visible || before.cursor.blink != after.cursor.blink
        || before.cursor.shape != after.cursor.shape;
}

bool TerminalCursorAnimation::shouldAnimate(
    const std::optional<Cursor> &cursor,
    bool liveViewport,
    bool sessionRunning,
    bool itemVisible,
    bool windowActive,
    bool focused,
    bool compositionActive) {
    return cursor && cursor->visible && cursor->blink && liveViewport
        && sessionRunning && itemVisible && windowActive && focused
        && !compositionActive;
}

bool TerminalCursorAnimation::withinHold(qint64 elapsedMs) {
    return elapsedMs >= 0 && elapsedMs < kCursorBlinkHoldMs;
}

bool TerminalCursorAnimation::shouldRunFadeTimer(bool cursorBlinkActive,
                                                 bool contentBusy,
                                                 bool withinHold) {
    return cursorBlinkActive && !contentBusy && withinHold;
}

} // namespace span::terminal
