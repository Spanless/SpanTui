#pragma once

#include "terminal/api/terminal_types.h"
#include "terminal/render/terminal_font_set.h"

#include <QColor>
#include <QFont>
#include <QPointF>
#include <QRectF>
#include <QVector>

namespace span::terminal {

struct TerminalGlyphCell {
    QString text;
    QFont font;
    QColor color;
    QRectF cell;
    int firstColumn = 0;
    int columns = 0;
};

struct TerminalBackgroundRun {
    QRectF rect;
    QColor color;
};

struct TerminalCustomGlyph {
    QString text;
    QRectF cell;
    QColor color;
};

struct TerminalDecoration {
    QRectF cell;
    // Strike-through and overline ink; the underline has its own (SGR 58).
    QColor color;
    QColor underlineColor;
    UnderlineStyle underline = UnderlineStyle::None;
    bool strike = false;
    bool overline = false;
};

struct TerminalRowLayout {
    QVector<TerminalGlyphCell> glyphCells;
    QVector<TerminalBackgroundRun> backgroundRuns;
    QVector<TerminalCustomGlyph> customGlyphs;
    QVector<TerminalDecoration> decorations;
};

class TerminalRowLayoutBuilder final {
public:
    static TerminalRowLayout build(const Line &line,
                                   const TerminalFontSet &fonts,
                                   const QColor &foreground,
                                   const QColor &background,
                                   const QVector<QColor> &ansiPalette,
                                   bool blinkVisible = true);
};

} // namespace span::terminal
