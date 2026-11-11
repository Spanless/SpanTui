#pragma once

#include <QtGlobal>

namespace span::terminal {

// The view's scroll position over history and the active screen, in rows
// from the oldest history row (the frame scrollbar's unit) with a fraction
// for partly scrolled rows. At the bottom it follows the active screen;
// elsewhere the top row keeps its number, so appended output never moves the
// text being read. Pure logic on a caller-supplied clock.
class TerminalScrollModel final {
public:
    // History occupies [firstLine, liveTop); the live screen starts at
    // liveTop, the bottom-most position. Returns whether top() changed.
    bool setRange(qint64 firstLine, qint64 liveTop);
    qint64 firstLine() const { return m_firstLine; }
    qint64 liveTop() const { return m_liveTop; }

    double top() const { return m_top; }
    bool atBottom() const { return m_following; }
    // Lines between the top edge and the live screen.
    double offset() const { return double(m_liveTop) - m_top; }

    // Immediate moves; they end any animation. Return whether top() changed.
    bool scrollTo(double top);
    bool scrollToBottom();
    // Moves the target by `lines` (negative: toward history). Animated unless
    // animation is off, then immediate.
    bool animateBy(double lines, qint64 nowMs);
    // Advances an animation to `nowMs`; returns whether top() changed.
    bool advance(qint64 nowMs);
    bool animating() const { return m_animating; }
    // Reduced motion turns animation off.
    void setAnimated(bool animated);
    void reset();

private:
    double clamped(double top) const;
    bool moveTo(double top);

    qint64 m_firstLine = 0;
    qint64 m_liveTop = 0;
    double m_top = 0;
    bool m_following = true;
    bool m_animated = true;
    bool m_animating = false;
    // The target follows the live screen when it is the bottom.
    double m_target = 0;
    bool m_targetBottom = false;
    qint64 m_lastMs = 0;
};

} // namespace span::terminal
