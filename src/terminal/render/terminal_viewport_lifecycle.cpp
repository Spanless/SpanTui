#include "terminal/render/terminal_viewport_lifecycle.h"

#include <algorithm>
#include <cmath>

namespace span::terminal {

void TerminalViewportLifecycle::reset()
{
    m_committedColumns = 0;
    m_committedRows = 0;
    m_commitPending = true;
}

void TerminalViewportLifecycle::setState(State state)
{
    if (m_state == state) {
        return;
    }
    m_state = state;
}

void TerminalViewportLifecycle::setContentSize(const QSizeF &size)
{
    const QSizeF normalized(std::max<qreal>(0.0, size.width()),
                            std::max<qreal>(0.0, size.height()));
    if (m_pendingPixels == normalized) {
        return;
    }
    m_pendingPixels = normalized;
    if (m_state != State::Closing) {
        m_commitPending = true;
    }
}

void TerminalViewportLifecycle::setCellSize(const QSizeF &size)
{
    if (m_cellSize == size) {
        return;
    }
    m_cellSize = size;
    if (m_state != State::Closing) {
        m_commitPending = true;
    }
}

TerminalViewportLifecycle::Commit TerminalViewportLifecycle::takeStableCommit()
{
    if (m_state != State::Normal || !m_commitPending) {
        return {};
    }
    const int columns = gridDimension(m_pendingPixels.width(), m_cellSize.width());
    const int rows = gridDimension(m_pendingPixels.height(), m_cellSize.height());
    if (columns == 0 || rows == 0) {
        return {};
    }

    Commit commit;
    commit.pixelSize = m_pendingPixels;
    commit.columns = columns;
    commit.rows = rows;
    commit.gridChanged = columns != m_committedColumns || rows != m_committedRows;
    m_committedColumns = columns;
    m_committedRows = rows;
    m_commitPending = false;
    return commit;
}

int TerminalViewportLifecycle::gridDimension(qreal pixels, qreal cellSize)
{
    if (pixels <= 0.0 || cellSize <= 0.0) {
        return 0;
    }
    return std::max(2, int(std::floor(pixels / cellSize)));
}

} // namespace span::terminal
