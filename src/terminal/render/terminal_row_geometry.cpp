#include "terminal/render/terminal_row_geometry.h"

#include <algorithm>
#include <limits>

namespace span::terminal {

namespace {

// Indices are 16 bit; a row that would need more drops what does not fit.
constexpr qsizetype kMaxVertices = std::numeric_limits<quint16>::max();

struct Ink {
    uchar r = 0;
    uchar g = 0;
    uchar b = 0;
    uchar a = 0;
};

Ink premultiplied(const QColor &color, int coverage = 255)
{
    const int alpha = (color.alpha() * coverage + 127) / 255;
    const QRgb rgba = qPremultiply(qRgba(color.red(), color.green(), color.blue(), alpha));
    return {uchar(qRed(rgba)), uchar(qGreen(rgba)), uchar(qBlue(rgba)), uchar(qAlpha(rgba))};
}

class Writer {
public:
    Writer(TerminalRowGeometry &geometry, const TerminalGlyphAtlas &atlas)
        : m_geometry(geometry),
          m_maskScale(1.0 / atlas.size(TerminalGlyphAtlas::Page::Mask)),
          m_colorScale(1.0 / atlas.size(TerminalGlyphAtlas::Page::Color)),
          m_solid(TerminalGlyphAtlas::solidTexel() * m_maskScale)
    {}

    void quad(const QRectF &rect, const QRectF &texels, Ink ink, bool field = false, bool color = false)
    {
        if (rect.isEmpty() || !reserve(4)) return;
        const auto first = quint16(m_geometry.vertices.size());
        const qreal scale = color ? m_colorScale : m_maskScale;
        const float left = float(texels.left() * scale);
        const float top = float(texels.top() * scale);
        const float right = float(texels.right() * scale);
        const float bottom = float(texels.bottom() * scale);
        vertex(rect.left(), rect.top(), left, top, ink, field, color);
        vertex(rect.right(), rect.top(), right, top, ink, field, color);
        vertex(rect.left(), rect.bottom(), left, bottom, ink, field, color);
        vertex(rect.right(), rect.bottom(), right, bottom, ink, field, color);
        m_geometry.indices.append({first, quint16(first + 1), quint16(first + 2), quint16(first + 2),
                                   quint16(first + 1), quint16(first + 3)});
    }

    void solid(const QRectF &rect, const QColor &color)
    {
        if (rect.isEmpty() || !reserve(4)) return;
        const Ink ink = premultiplied(color);
        const auto first = quint16(m_geometry.vertices.size());
        const float u = float(m_solid.x());
        const float v = float(m_solid.y());
        vertex(rect.left(), rect.top(), u, v, ink);
        vertex(rect.right(), rect.top(), u, v, ink);
        vertex(rect.left(), rect.bottom(), u, v, ink);
        vertex(rect.right(), rect.bottom(), u, v, ink);
        m_geometry.indices.append({first, quint16(first + 1), quint16(first + 2), quint16(first + 2),
                                   quint16(first + 1), quint16(first + 3)});
    }

private:
    bool reserve(qsizetype count) const { return m_geometry.vertices.size() + count <= kMaxVertices; }

    void vertex(qreal x, qreal y, float u, float v, Ink ink, bool field = false, bool color = false)
    {
        m_geometry.vertices.push_back(
            {float(x), float(y), u, v, ink.r, ink.g, ink.b, ink.a, uchar(field ? 255 : 0), uchar(color ? 255 : 0), {}});
    }

    TerminalRowGeometry &m_geometry;
    const qreal m_maskScale;
    const qreal m_colorScale;
    const QPointF m_solid;
};

void writeDecoration(Writer &writer, const TerminalDecoration &decoration, qreal devicePixelRatio)
{
    const QRectF &cell = decoration.cell;
    const QColor &underlineColor = decoration.underlineColor;
    const qreal thickness = std::max<qreal>(1.0 / devicePixelRatio, cell.height() * 0.06);
    const qreal underlineY = cell.bottom() - std::max<qreal>(thickness, cell.height() * 0.12);
    if (decoration.underline == UnderlineStyle::Curly) {
        const qreal segment = std::max<qreal>(2.0 / devicePixelRatio, cell.width() / 8.0);
        for (qreal x = cell.left(); x < cell.right(); x += segment) {
            const qreal y = underlineY + (int((x - cell.left()) / segment) % 2 ? thickness : 0);
            writer.solid(QRectF(x, y, std::min(segment, cell.right() - x), thickness), underlineColor);
        }
    } else if (decoration.underline == UnderlineStyle::Dotted || decoration.underline == UnderlineStyle::Dashed) {
        const bool dotted = decoration.underline == UnderlineStyle::Dotted;
        const qreal dash = dotted ? thickness : std::max<qreal>(thickness * 3, cell.width() / 5);
        const qreal gap = dotted ? thickness : thickness * 1.5;
        for (qreal x = cell.left(); x < cell.right(); x += dash + gap) {
            writer.solid(QRectF(x, underlineY, std::min(dash, cell.right() - x), thickness), underlineColor);
        }
    } else if (decoration.underline != UnderlineStyle::None) {
        writer.solid(QRectF(cell.left(), underlineY, cell.width(), thickness), underlineColor);
        if (decoration.underline == UnderlineStyle::Double) {
            writer.solid(QRectF(cell.left(), underlineY - thickness * 2, cell.width(), thickness), underlineColor);
        }
    }
    if (decoration.strike) {
        writer.solid(QRectF(cell.left(), cell.center().y() - thickness / 2, cell.width(), thickness), decoration.color);
    }
    if (decoration.overline) {
        writer.solid(QRectF(cell.left(), cell.top(), cell.width(), thickness), decoration.color);
    }
}

void write(TerminalRowGeometry &geometry, const TerminalRowLayout &layout, TerminalGlyphCache &glyphs,
           qreal devicePixelRatio)
{
    geometry.vertices.clear();
    geometry.indices.clear();
    geometry.atlas = glyphs.atlas();
    Writer writer(geometry, *geometry.atlas);
    for (const TerminalBackgroundRun &background : layout.backgroundRuns)
        writer.solid(background.rect, background.color);
    for (const TerminalCustomGlyph &glyph : layout.customGlyphs) {
        const TerminalGlyph &chrome = glyphs.chrome(glyph);
        if (!chrome.texels.isNull()) {
            writer.quad(chrome.quad.translated(glyph.cell.topLeft()), chrome.texels, premultiplied(glyph.color));
        }
    }
    for (const TerminalGlyphCell &cell : layout.glyphCells) {
        const TerminalGlyph &glyph = glyphs.glyph(cell);
        if (glyph.texels.isNull()) continue;
        // Colour glyphs keep their pixels and take only the ink's opacity.
        const Ink ink = glyph.color ? Ink{uchar(cell.color.alpha()), uchar(cell.color.alpha()),
                                          uchar(cell.color.alpha()), uchar(cell.color.alpha())}
                                    : premultiplied(cell.color);
        writer.quad(glyph.quad.translated(cell.cell.topLeft()), glyph.texels, ink, glyph.field, glyph.color);
    }
    for (const TerminalDecoration &decoration : layout.decorations)
        writeDecoration(writer, decoration, devicePixelRatio);
}

} // namespace

TerminalRowGeometry TerminalRowGeometryBuilder::build(const TerminalRowLayout &layout, TerminalGlyphCache &glyphs,
                                                      qreal devicePixelRatio)
{
    TerminalRowGeometry geometry;
    write(geometry, layout, glyphs, devicePixelRatio);
    // A full atlas was replaced while this row was written; written again,
    // all of its glyphs come from the new one, which kept the ones just used.
    if (geometry.atlas != glyphs.atlas()) write(geometry, layout, glyphs, devicePixelRatio);
    return geometry;
}

} // namespace span::terminal
