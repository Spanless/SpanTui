#pragma once

#include "terminal/render/terminal_glyph_atlas.h"
#include "terminal/render/terminal_row_layout.h"

#include <QVector>

#include <memory>

namespace span::terminal {

// Vertex of the terminal cell shader: logical position, normalized atlas
// coordinate and premultiplied colour. Untextured quads sample the mask
// page's opaque texel, so the colour alone paints them. `field` is 255 for
// text glyphs, whose mask texels are a distance field the shader turns into
// coverage (see TerminalFieldText); `color` is 255 for colour glyphs, whose
// texels are on the colour page. Everything is texel times colour.
struct TerminalCellVertex {
    float x = 0;
    float y = 0;
    float u = 0;
    float v = 0;
    uchar r = 0;
    uchar g = 0;
    uchar b = 0;
    uchar a = 0;
    uchar field = 0;
    uchar color = 0;
    uchar padding[2] = {};
};
static_assert(sizeof(TerminalCellVertex) == 24, "TerminalCellVertex is uploaded as is");

// Everything one row paints, as one triangle list in paint order:
// backgrounds, box-drawing/block/braille chrome, text, decorations. Glyph
// texels refer to `atlas`.
struct TerminalRowGeometry {
    QVector<TerminalCellVertex> vertices;
    QVector<quint16> indices;
    std::shared_ptr<const TerminalGlyphAtlas> atlas;
};

class TerminalRowGeometryBuilder final {
public:
    // Rasterizes new glyphs into `glyphs` as needed. Vertex order depends on
    // the layout's geometry only, never its colours, so the layouts of two
    // blink phases give vertices that share one index list.
    static TerminalRowGeometry build(const TerminalRowLayout &layout, TerminalGlyphCache &glyphs,
                                     qreal devicePixelRatio);
};

} // namespace span::terminal
