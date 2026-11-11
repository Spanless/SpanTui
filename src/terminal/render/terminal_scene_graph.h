#pragma once

#include "terminal/api/terminal_types.h"
#include "terminal/render/terminal_frame_preparer.h"
#include "terminal/render/terminal_ime_composition_state.h"

#include <QColor>
#include <QFont>
#include <QVector>
#include <QRectF>

#include <optional>

class QQuickWindow;
class QSGNode;

namespace span::terminal {

class TerminalSceneGraph final {
public:
    struct PreeditPlacement {
        qreal originX = 0;
        qreal textWidth = 0;
        qreal cursorX = 0;
    };

    static PreeditPlacement placePreedit(const TerminalImeCompositionState &composition,
                                         const QFont &font,
                                         qreal anchorX,
                                         qreal cellWidth,
                                         qreal viewportWidth);

    // Placement relative to the viewport's first row, before clipping.
    // libghostty sizes placements in the whole-pixel cell size the view
    // reports (TerminalSession::setCellPixelSize); they scale with the
    // fractional cell actually drawn.
    static QRectF graphicRect(const GraphicPlacement &graphic, qreal cellWidth, qreal cellHeight);

    struct PaintState {
        quint64 sceneGeneration = 0;
        quint64 graphicsRevision = 0;
        quint64 contentPresentationSequence = 0;
        quint64 lastSnapshotSequence = 0;
        quint64 viewportEpoch = 0;
        // Drawn rows, top first: one more than the grid while the top row is
        // partly scrolled out. Prepared off the GUI thread; a row is repainted
        // when its pointer differs from the one its node presents, and a null
        // row (not prepared yet) draws nothing.
        const QVector<std::shared_ptr<const TerminalPreparedRow>> *rows = nullptr;
        // Y of the first drawn row, in (-cellHeight, 0].
        qreal rowOffset = 0;
        // Drawn-row index of the frame viewport's first row. Cursor, preedit
        // and graphics are placed relative to it.
        int liveRow = 0;
        bool blinkVisible = true;
        // The frame drawn; its grid rows bound the clip.
        const Frame *frame = nullptr;
        qreal cellWidth = 0;
        qreal cellHeight = 0;
        qreal baseline = 0;
        qreal devicePixelRatio = 1;
        qreal itemWidth = 0;
        // Selection and search marks, `line` being the drawn row; both are
        // painted under the text.
        QVector<TerminalHighlight> selectionHighlights;
        QColor selectionColor;
        QVector<TerminalHighlight> searchHighlights;
        QColor searchMatchColor;
        QColor searchCurrentColor;
        bool showCursor = false;
        std::optional<Cursor> visualCursor;
        CursorShape cursorShape = CursorShape::Block;
        QColor cursorColor;
        qreal cursorOpacity = 1;
        bool paintPreedit = false;
        bool traceEnabled = false;
        const TerminalImeCompositionState *imeComposition = nullptr;
        QColor foreground;
        QColor background;
        QFont preeditFont;
        QQuickWindow *window = nullptr;
    };

    struct PaintResult {
        bool contentSequenceAdvanced = false;
        bool viewportTraceChanged = false;
        qsizetype rowCount = 0;
        int rowChildCount = 0;
        // Rows whose nodes took new vertices in this sync.
        int updatedRows = 0;
    };

    static QSGNode *sync(QSGNode *oldNode, const PaintState &state, PaintResult &result);
};

} // namespace span::terminal
