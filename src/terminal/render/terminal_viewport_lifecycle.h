#pragma once

#include <QSizeF>

namespace span::terminal {

// Owns only the presentation-side lifecycle.  It deliberately knows nothing
// about QML positions, animations, ConPTY, or frame contents.
class TerminalViewportLifecycle final {
public:
    enum class State {
        Normal = 0,
        Moving,
        Hidden,
        Closing,
    };

    struct Commit {
        QSizeF pixelSize;
        int columns = 0;
        int rows = 0;
        bool gridChanged = false;

        explicit operator bool() const { return columns > 0 && rows > 0; }
    };

    State state() const { return m_state; }
    void reset();
    void setState(State state);
    void setContentSize(const QSizeF &size);
    void setCellSize(const QSizeF &size);
    Commit takeStableCommit();

private:
    static int gridDimension(qreal pixels, qreal cellSize);

    State m_state = State::Normal;
    QSizeF m_pendingPixels;
    QSizeF m_cellSize;
    int m_committedColumns = 0;
    int m_committedRows = 0;
    bool m_commitPending = true;
};

} // namespace span::terminal
