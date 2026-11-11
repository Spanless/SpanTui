#include "terminal/render/terminal_scroll_model.h"

#include "terminal/terminal_constants.h"

#include <algorithm>
#include <cmath>

namespace span::terminal {

bool TerminalScrollModel::setRange(qint64 firstLine, qint64 liveTop)
{
    m_firstLine = std::min(firstLine, liveTop);
    m_liveTop = liveTop;
    if (m_animating && m_targetBottom) m_target = double(m_liveTop);
    m_target = clamped(m_target);
    // Following keeps the bottom; otherwise the same line stays on top until
    // its row leaves the history.
    return moveTo(m_following ? double(m_liveTop) : m_top);
}

bool TerminalScrollModel::scrollTo(double top)
{
    m_animating = false;
    return moveTo(top);
}

bool TerminalScrollModel::scrollToBottom()
{
    return scrollTo(double(m_liveTop));
}

bool TerminalScrollModel::animateBy(double lines, qint64 nowMs)
{
    const double from = m_animating ? m_target : m_top;
    const double target = clamped(from + lines);
    if (!m_animated) return scrollTo(target);
    m_target = target;
    m_targetBottom = target >= double(m_liveTop);
    if (!m_animating) {
        m_animating = true;
        m_lastMs = nowMs;
    }
    // A target that is where the view already is ends nothing new.
    if (std::abs(m_target - m_top) < kScrollSettleLines) return scrollTo(m_target);
    return false;
}

bool TerminalScrollModel::advance(qint64 nowMs)
{
    if (!m_animating) return false;
    const double elapsed = double(std::clamp<qint64>(nowMs - m_lastMs, 0, kScrollMaxFrameMs));
    m_lastMs = nowMs;
    const double remaining = m_target - m_top;
    if (std::abs(remaining) < kScrollSettleLines) return scrollTo(m_target);
    const double next = m_top + remaining * (1.0 - std::exp(-elapsed / kScrollTimeConstantMs));
    if (std::abs(m_target - next) < kScrollSettleLines) return scrollTo(m_target);
    return moveTo(next);
}

void TerminalScrollModel::setAnimated(bool animated)
{
    m_animated = animated;
    if (!animated && m_animating) scrollTo(m_target);
}

void TerminalScrollModel::reset()
{
    m_firstLine = 0;
    m_liveTop = 0;
    m_top = 0;
    m_following = true;
    m_animating = false;
    m_target = 0;
    m_targetBottom = false;
}

double TerminalScrollModel::clamped(double top) const
{
    return std::clamp(top, double(m_firstLine), double(m_liveTop));
}

bool TerminalScrollModel::moveTo(double top)
{
    const double next = clamped(top);
    m_following = next >= double(m_liveTop);
    if (next == m_top) return false;
    m_top = next;
    return true;
}

} // namespace span::terminal
