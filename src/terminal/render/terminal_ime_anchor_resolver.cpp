#include "terminal/render/terminal_ime_anchor_resolver.h"

#include <algorithm>

namespace span::terminal {

namespace {

bool cursorIsInside(const Cursor &cursor, const Frame &frame) {
    return cursor.row >= 0 && cursor.row < frame.rows
        && cursor.column >= 0 && cursor.column < frame.columns;
}

struct ReverseRunCandidate {
    int row = 0;
    int column = 0;
};

QVector<ReverseRunCandidate> singleLogicalCellReverseRuns(const Frame &frame) {
    QVector<ReverseRunCandidate> candidates;
    for (int row = 0; row < frame.rows && frame.viewportRow + row < frame.lines.size(); ++row) {
        const QVector<Cell> &cells = frame.lines.at(frame.viewportRow + row).cells;
        const int lineColumns = std::min(frame.columns, int(cells.size()));
        int column = 0;
        while (column < lineColumns) {
            const Cell &cell = cells.at(column);
            if (cell.continuation) {
                ++column;
                continue;
            }

            const int occupiedColumns = std::max(1, cell.width);
            if (!cell.style.reverse) {
                column += occupiedColumns;
                continue;
            }

            const int runStart = column;
            int logicalCells = 0;
            while (column < lineColumns) {
                const Cell &runCell = cells.at(column);
                if (runCell.continuation) {
                    ++column;
                    continue;
                }
                if (!runCell.style.reverse) {
                    break;
                }
                ++logicalCells;
                column += std::max(1, runCell.width);
            }
            if (logicalCells == 1) {
                candidates.append({row, runStart});
            }
        }
    }
    return candidates;
}

} // namespace

TerminalCursorResolution TerminalCursorResolver::resolve(const Frame &frame, quint64 frameTransportEpoch,
                                                         bool liveViewport)
{
    if (!liveViewport || frameTransportEpoch == 0 || !cursorIsInside(frame.cursor, frame)) {
        return {};
    }

    TerminalCursorResolution result;
    if (frame.cursor.visible) {
        result.visualCursor = frame.cursor;
        result.imeAnchor = {frame.cursor,
                            TerminalImeAnchorSource::ProtocolCursor};
        return result;
    }

    const QVector<ReverseRunCandidate> candidates =
        singleLogicalCellReverseRuns(frame);
    if (candidates.size() == 1) {
        Cursor imeCursor = frame.cursor;
        imeCursor.row = candidates.first().row;
        imeCursor.column = candidates.first().column;
        result.imeAnchor = {imeCursor, TerminalImeAnchorSource::ReverseVideoCell};
        return result;
    }

    // A hidden protocol cursor is a parser pen, not evidence of a visible IME
    // insertion point. Ambiguous or absent software carets must not be guessed.
    return result;
}

} // namespace span::terminal
