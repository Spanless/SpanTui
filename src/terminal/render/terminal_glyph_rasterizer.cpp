#include "terminal/render/terminal_glyph_rasterizer.h"

#include "terminal/render/terminal_drawn_glyphs.h"
#include "terminal/render/terminal_triangle_raster.h"

#include <QGlyphRun>
#include <QPainter>
#include <QPainterPath>
#include <QTextLayout>
#include <QTextOption>
#include <QtGui/private/qdistancefield_p.h>
#include <QtGui/private/qfontengine_p.h>
#include <QtGui/private/qrawfont_p.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>

namespace span::terminal {

namespace {

// Channel difference above which a pixel counts as coloured.
constexpr int kColorTolerance = 8;

// Whether white ink left anything but grey (r = g = b = a): a colour glyph.
bool hasColor(const QImage &pixels)
{
    for (int y = 0; y < pixels.height(); ++y) {
        const uchar *line = pixels.constScanLine(y);
        for (int x = 0; x < pixels.width(); ++x) {
            const uchar *pixel = line + x * 4;
            if (std::abs(pixel[0] - pixel[3]) > kColorTolerance || std::abs(pixel[1] - pixel[3]) > kColorTolerance
                || std::abs(pixel[2] - pixel[3]) > kColorTolerance) {
                return true;
            }
        }
    }
    return false;
}

} // namespace

TerminalGlyphRasterizer::TerminalGlyphRasterizer(const TerminalFontSet &fonts)
    : m_devicePixelRatio(fonts.devicePixelRatio()),
      m_cellHeight(fonts.cellHeight()),
      m_baseline(fonts.baseline())
{}

int TerminalGlyphRasterizer::deviceWidth(const TerminalGlyphCell &cell) const
{
    return int(std::lround(cell.cell.width() * m_devicePixelRatio));
}

TerminalGlyphRaster TerminalGlyphRasterizer::text(const TerminalGlyphCell &cell)
{
    const qreal dpr = m_devicePixelRatio;
    const int width = deviceWidth(cell);
    // The cell's font may be in use on another thread.
    QFont font = TerminalFontSet::isolatedCopy(cell.font);
    // Decorations are drawn as lines of their own. Painted glyphs keep grey
    // antialiasing, valid on any background.
    font.setUnderline(false);
    font.setStrikeOut(false);
    font.setStyleStrategy(QFont::StyleStrategy(font.styleStrategy() | QFont::NoSubpixelAntialias));
    QTextLayout layout(cell.text, font);
    QTextOption option;
    option.setWrapMode(QTextOption::NoWrap);
    layout.setTextOption(option);
    layout.beginLayout();
    QTextLine line = layout.createLine();
    if (line.isValid()) {
        line.setLineWidth(std::numeric_limits<qreal>::max());
        line.setPosition(QPointF());
    }
    layout.endLayout();
    if (!line.isValid()) return {};

    // Wide glyphs are centred in their cells; overhang (italics, large
    // symbols) gets a cell height of room on every side.
    const qreal spanWidth = width / dpr;
    const qreal centre = cell.columns > 1
        ? TerminalGlyphGeometry::snap(std::max<qreal>(0.0, (spanWidth - line.naturalTextWidth()) / 2.0), dpr)
        : 0.0;
    // The baseline sits on a device pixel, so every row draws a glyph alike.
    const qreal baseline = TerminalGlyphGeometry::snap(m_baseline, dpr);
    if (std::optional<TerminalGlyphRaster> outlines = field(layout, QPointF(centre, baseline - line.ascent()))) {
        return *outlines;
    }

    // Colour fonts are painted in white.
    const int pad = int(std::ceil(m_cellHeight * dpr));
    const QSize canvas(width + 2 * pad, int(std::ceil(m_cellHeight * dpr)) + 2 * pad);
    QImage image(canvas, QImage::Format_ARGB32_Premultiplied);
    image.fill(Qt::transparent);
    image.setDevicePixelRatio(dpr);
    {
        QPainter painter(&image);
        painter.setPen(Qt::white);
        const qreal origin = pad / dpr;
        layout.draw(&painter, QPointF(origin + centre, origin + baseline - line.ascent()));
    }
    const QImage pixels = image.convertToFormat(QImage::Format_RGBA8888_Premultiplied);
    return inked(pixels, QPoint(-pad, -pad), hasColor(pixels));
}

std::optional<TerminalGlyphRaster> TerminalGlyphRasterizer::field(const QTextLayout &layout, QPointF origin)
{
    const QList<QGlyphRun> runs = layout.glyphRuns();
    if (runs.isEmpty()) return TerminalGlyphRaster{};
    bool narrow = true;
    for (const QGlyphRun &run : runs) {
        const QFontEngine *engine = QRawFontPrivate::get(run.rawFont())->fontEngine;
        if (!engine || engine->isColorFont()) return std::nullopt;
        narrow = narrow && narrowOutlines(run.rawFont());
    }

    // The outlines at the generator's size, all on one grid: a base glyph and
    // its marks make one field.
    const int scale = QT_DISTANCEFIELD_SCALE(narrow);
    const qreal unitsPerPixel =
        qreal(QT_DISTANCEFIELD_BASEFONTSIZE(narrow)) * scale / runs.constFirst().rawFont().pixelSize();
    QPainterPath outline;
    for (const QGlyphRun &run : runs) {
        const QRawFont font = outlineFont(run.rawFont(), run.rawFont().pixelSize() * unitsPerPixel);
        const QList<quint32> indexes = run.glyphIndexes();
        const QList<QPointF> positions = run.positions();
        for (qsizetype glyph = 0; glyph < indexes.size(); ++glyph) {
            outline.addPath(
                font.pathForGlyph(indexes.at(glyph)).translated((origin + positions.at(glyph)) * unitsPerPixel));
        }
    }
    if (outline.isEmpty()) return TerminalGlyphRaster{};
    outline.setFillRule(Qt::WindingFill);

    // The generator places the outline's bounds a margin inside the field;
    // the quad maps the field back onto the cell.
    const QDistanceField distanceField(outline, 0, narrow);
    const qreal texel = scale / unitsPerPixel;
    const qreal margin = QT_DISTANCEFIELD_RADIUS(narrow) / qreal(scale) * texel;
    const QRectF bounds = outline.boundingRect();

    // Empty texels around the field, as far as the subpixel shader samples
    // past the quad (two thirds of a device pixel, and the filter's
    // neighbour), keep other glyphs out of it.
    const int padding = int(std::ceil(0.667 / (texel * m_devicePixelRatio))) + 1;
    const QImage distances = distanceField.toImage(QImage::Format_Alpha8);
    TerminalGlyphRaster raster;
    raster.image = QImage(distances.width() + 2 * padding, distances.height() + 2 * padding, QImage::Format_Alpha8);
    raster.image.fill(0);
    for (int y = 0; y < distances.height(); ++y) {
        std::memcpy(raster.image.scanLine(padding + y) + padding, distances.constScanLine(y),
                    size_t(distances.width()));
    }
    raster.glyph.quad = QRectF(bounds.left() / unitsPerPixel - margin, bounds.top() / unitsPerPixel - margin,
                               distanceField.width() * texel, distanceField.height() * texel);
    raster.glyph.texels = QRectF(QPointF(padding, padding), distances.size());
    raster.glyph.field = true;
    return raster;
}

QRawFont TerminalGlyphRasterizer::outlineFont(const QRawFont &font, qreal pixelSize)
{
    // Resizing makes a font engine, which DirectWrite takes its time over.
    const std::pair<QRawFont, qreal> key{font, pixelSize};
    const auto found = m_outlineFonts.constFind(key);
    if (found != m_outlineFonts.cend()) return *found;
    QRawFont resized = font;
    resized.setPixelSize(pixelSize);
    return *m_outlineFonts.insert(key, resized);
}

bool TerminalGlyphRasterizer::narrowOutlines(const QRawFont &font)
{
    // Qt Quick's rule: fine outlines get fields of double resolution, unless
    // the font has too many glyphs for that.
    const auto found = m_narrowOutlines.constFind(font);
    if (found != m_narrowOutlines.cend()) return *found;
    const QFontEngine *engine = QRawFontPrivate::get(font)->fontEngine;
    const bool narrow = qt_fontHasNarrowOutlines(font) && engine->glyphCount() < QT_DISTANCEFIELD_HIGHGLYPHCOUNT();
    return *m_narrowOutlines.insert(font, narrow);
}

TerminalGlyphRaster TerminalGlyphRasterizer::inked(const QImage &canvas, QPoint origin, bool color) const
{
    // Coverage keeps one byte a pixel: white's alpha.
    const QImage pixels =
        color || canvas.format() == QImage::Format_Alpha8 ? canvas : canvas.convertToFormat(QImage::Format_Alpha8);
    const int depth = pixels.depth() / 8;
    int left = pixels.width();
    int top = pixels.height();
    int right = -1;
    int bottom = -1;
    for (int y = 0; y < pixels.height(); ++y) {
        const uchar *line = pixels.constScanLine(y);
        for (int x = 0; x < pixels.width(); ++x) {
            const uchar *pixel = line + x * depth;
            if (std::all_of(pixel, pixel + depth, [](uchar byte) { return byte == 0; })) continue;
            left = std::min(left, x);
            right = std::max(right, x);
            top = std::min(top, y);
            bottom = std::max(bottom, y);
        }
    }
    TerminalGlyphRaster raster;
    if (right < 0) return raster;
    const qreal dpr = m_devicePixelRatio;
    const QRect inked(QPoint(left, top), QPoint(right, bottom));
    raster.image = pixels.copy(inked);
    raster.glyph.quad =
        QRectF((left + origin.x()) / dpr, (top + origin.y()) / dpr, inked.width() / dpr, inked.height() / dpr);
    raster.glyph.texels = QRectF(QPointF(), inked.size());
    raster.glyph.color = color;
    return raster;
}

TerminalGlyphRaster TerminalGlyphRasterizer::chrome(const TerminalCustomGlyph &glyph) const
{
    const qreal dpr = m_devicePixelRatio;
    // Cells start on device pixels, so pixels drawn at the origin are exact
    // anywhere once translated.
    const QRectF cell(0, 0, glyph.cell.width(), glyph.cell.height());
    QVector<TerminalCoverageMesh> meshes;
    QVector<QRectF> rects;
    if (TerminalBoxDrawing::supports(glyph.text)) {
        const TerminalBoxDrawingGeometry geometry = TerminalBoxDrawing::geometry(glyph.text, cell, dpr);
        for (const TerminalStrokePath &stroke : geometry.strokes) {
            meshes.push_back(TerminalStrokeMeshBuilder::build(stroke.centerLine, stroke.normals, stroke.width, dpr));
        }
    } else if (TerminalBraille::supports(glyph.text)) {
        meshes.push_back(TerminalBrailleMeshBuilder::build(TerminalBraille::geometry(glyph.text, cell, dpr)));
    } else {
        rects = TerminalBlockElements::rectangles(glyph.text, cell, dpr);
    }

    // Antialiasing fringes may reach past the cell; the canvas spans them.
    qreal left = std::numeric_limits<qreal>::max();
    qreal top = left;
    qreal right = std::numeric_limits<qreal>::lowest();
    qreal bottom = right;
    const auto include = [&](qreal x, qreal y) {
        left = std::min(left, x);
        top = std::min(top, y);
        right = std::max(right, x);
        bottom = std::max(bottom, y);
    };
    for (const TerminalCoverageMesh &mesh : meshes) {
        for (const TerminalCoverageMeshVertex &vertex : mesh.vertices)
            include(vertex.position.x(), vertex.position.y());
    }
    for (const QRectF &rect : rects) {
        include(rect.left(), rect.top());
        include(rect.right(), rect.bottom());
    }
    if (!(right > left && bottom > top)) return {};
    const QPoint origin(int(std::floor(left * dpr)), int(std::floor(top * dpr)));
    const QSize size(int(std::ceil(right * dpr)) - origin.x(), int(std::ceil(bottom * dpr)) - origin.y());
    QImage canvas(size, QImage::Format_Alpha8);
    canvas.fill(0);
    // Coverage, source-over, as the GPU blended the geometry before.
    const auto cover = [&canvas](int x, int y, float coverage) {
        if (coverage <= 0) return;
        uchar *pixel = canvas.scanLine(y) + x;
        const float value = coverage + *pixel * (255.0f - coverage) / 255.0f;
        *pixel = uchar(std::clamp(std::lround(value), 0L, 255L));
    };
    for (const TerminalCoverageMesh &mesh : meshes) {
        for (qsizetype index = 0; index + 2 < mesh.indices.size(); index += 3) {
            const TerminalCoverageMeshVertex *corner[3] = {&mesh.vertices.at(mesh.indices.at(index)),
                                                           &mesh.vertices.at(mesh.indices.at(index + 1)),
                                                           &mesh.vertices.at(mesh.indices.at(index + 2))};
            float x[3];
            float y[3];
            for (int k = 0; k < 3; ++k) {
                x[k] = float(corner[k]->position.x() * dpr - origin.x());
                y[k] = float(corner[k]->position.y() * dpr - origin.y());
            }
            rasterizeTriangle(x, y, size.width(), size.height(), [&](int px, int py, const float (&weights)[3]) {
                cover(px, py,
                      weights[0] * corner[0]->coverage + weights[1] * corner[1]->coverage
                          + weights[2] * corner[2]->coverage);
            });
        }
    }
    for (const QRectF &rect : rects) {
        // Pixels whose centres the rectangle covers: top and left edges in,
        // bottom and right out.
        const QRectF device(rect.left() * dpr - origin.x(), rect.top() * dpr - origin.y(), rect.width() * dpr,
                            rect.height() * dpr);
        const int firstX = std::max(0, int(std::ceil(device.left() - 0.5)));
        const int endX = std::min(size.width(), int(std::ceil(device.right() - 0.5)));
        const int firstY = std::max(0, int(std::ceil(device.top() - 0.5)));
        const int endY = std::min(size.height(), int(std::ceil(device.bottom() - 0.5)));
        for (int py = firstY; py < endY; ++py) {
            for (int px = firstX; px < endX; ++px) cover(px, py, 255.0f);
        }
    }
    return inked(canvas, origin, false);
}

} // namespace span::terminal
