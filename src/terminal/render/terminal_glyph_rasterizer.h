#pragma once

#include "terminal/render/terminal_font_set.h"
#include "terminal/render/terminal_row_layout.h"

#include <QHash>
#include <QImage>
#include <QPoint>
#include <QRawFont>
#include <QRectF>

#include <optional>

class QTextLayout;

namespace span::terminal {

// Where a cell's glyph is drawn, relative to the cell's top-left corner, and
// which atlas texels it shows. An empty quad draws nothing.
struct TerminalGlyph {
    QRectF quad;
    QRectF texels;
    // Texels are a distance field on the mask page, scaled onto the quad;
    // otherwise one texel a device pixel.
    bool field = false;
    // Texels are on the colour page and carry their own colour (emoji).
    // Otherwise they are coverage for the cell's ink.
    bool color = false;
};

// A glyph not placed in an atlas yet: its texels are relative to `image`
// (Format_Alpha8, or RGBA8888 premultiplied for a colour glyph).
struct TerminalGlyphRaster {
    TerminalGlyph glyph;
    QImage image;
};

// Turns cells into texels for the glyph atlas: text into distance fields of
// its outlines from Qt Quick's generator (see TerminalFieldText), painted
// pixels for colour fonts; box-drawing, block and braille cells into their
// coverage, pixel for pixel as the GPU would have drawn their geometry.
// The fonts it keeps belong to the thread that made them (QRawFont), so
// every thread rasterizes with a rasterizer of its own, which that thread
// also destroys.
class TerminalGlyphRasterizer final {
public:
    explicit TerminalGlyphRasterizer(const TerminalFontSet &fonts);

    // Device pixels of the cell's width: with the text and font, all a
    // cell's texels depend on.
    int deviceWidth(const TerminalGlyphCell &cell) const;

    TerminalGlyphRaster text(const TerminalGlyphCell &cell);
    TerminalGlyphRaster chrome(const TerminalCustomGlyph &glyph) const;

private:
    // One distance field of the laid out text's outlines, its pen at
    // `origin` (logical, from the cell's top-left corner); nothing when a
    // run's font is a colour font.
    std::optional<TerminalGlyphRaster> field(const QTextLayout &layout, QPointF origin);
    // `font` at the size its outlines are turned into fields at.
    QRawFont outlineFont(const QRawFont &font, qreal pixelSize);
    // Whether Qt Quick would give this font's fields double resolution.
    bool narrowOutlines(const QRawFont &font);
    // The inked part of a canvas whose top-left pixel sits `origin` device
    // pixels from the cell's top-left corner.
    TerminalGlyphRaster inked(const QImage &canvas, QPoint origin, bool color) const;

    qreal m_devicePixelRatio = 1;
    qreal m_cellHeight = 0;
    qreal m_baseline = 0;
    QHash<QRawFont, bool> m_narrowOutlines;
    QHash<std::pair<QRawFont, qreal>, QRawFont> m_outlineFonts;
};

} // namespace span::terminal
