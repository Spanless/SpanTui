#pragma once

#include "terminal/render/terminal_glyph_atlas.h"

#include <QImage>
#include <QRectF>
#include <QSGGeometry>
#include <QSGMaterial>
#include <QSGTexture>
#include <QVector>

#include <memory>
#include <utility>

class QRhiTexture;

namespace span::terminal {

struct TerminalCellVertex;

// Qt Quick's distance-field text (QSGDistanceFieldGlyphNode), which the rest
// of the window is drawn with. Glyph fields come from Qt's own generator
// (TerminalGlyphRasterizer); a field texel holds 0.5 on the outline, more inside,
// less outside. The cell shaders turn it into coverage with the thresholds
// below; this is the same on the CPU, for the software renderer.
struct TerminalFieldText {
    // Smoothstep edges for a field drawn `scale` device pixels per field
    // texel: Qt's defaults, a little bolder for small text.
    static std::pair<float, float> alphaRange(float scale);
    // Greyscale coverage of a field sample (shaders/terminal_cell.frag).
    static float coverage(float distance, float scale);
};

// Render-thread texture of one glyph atlas page. Each commit uploads only
// the pixels placed since the previous one.
class TerminalAtlasTexture final : public QSGTexture {
public:
    TerminalAtlasTexture(std::shared_ptr<const TerminalGlyphAtlas> atlas, TerminalGlyphAtlas::Page page);
    ~TerminalAtlasTexture() override;

    const TerminalGlyphAtlas *atlas() const { return m_atlas.get(); }

    qint64 comparisonKey() const override;
    QRhiTexture *rhiTexture() const override;
    QSize textureSize() const override;
    bool hasAlphaChannel() const override { return true; }
    bool hasMipmaps() const override { return false; }
    void commitTextureOperations(QRhi *rhi, QRhiResourceUpdateBatch *resourceUpdates) override;

private:
    std::shared_ptr<const TerminalGlyphAtlas> m_atlas;
    TerminalGlyphAtlas::Page m_page;
    QRhiTexture *m_texture = nullptr;
    quint64 m_uploaded = 0;
};

// Draws TerminalCellVertex triangles: atlas texel times the vertex colour,
// and text glyphs' distance fields the way Qt Quick draws its text
// (TerminalFieldText), with subpixel coverage where the backend blends
// dual-source. Rows are never merged into one batch, so scrolling moves a
// row by its matrix and uploads no vertices.
class TerminalCellMaterial final : public QSGMaterial {
public:
    // The textures of one atlas's mask and colour pages.
    TerminalCellMaterial(TerminalAtlasTexture *mask, TerminalAtlasTexture *colors, bool subpixel);

    // Whether this graphics API draws text with subpixel coverage.
    static bool supportsSubpixel(QSGRendererInterface::GraphicsApi api);
    static const QSGGeometry::AttributeSet &attributes();

    TerminalAtlasTexture *mask() const { return m_mask; }
    TerminalAtlasTexture *colors() const { return m_colors; }

    QSGMaterialType *type() const override;
    QSGMaterialShader *createShader(QSGRendererInterface::RenderMode renderMode) const override;
    int compare(const QSGMaterial *other) const override;

private:
    TerminalAtlasTexture *m_mask;
    TerminalAtlasTexture *m_colors;
    bool m_subpixel;
};

// CPU twin of the greyscale shader for the software scene graph, which
// draws no custom material. Mirrors the atlas pages from their uploads.
class TerminalCellRasterizer final {
public:
    explicit TerminalCellRasterizer(std::shared_ptr<const TerminalGlyphAtlas> atlas);

    // The triangles in device pixels on a transparent image; `bounds` gets
    // the logical rectangle the image covers. Null when nothing is drawn.
    QImage paint(const QVector<TerminalCellVertex> &vertices, const QVector<quint16> &indices, qreal devicePixelRatio,
                 QRectF *bounds);

private:
    std::shared_ptr<const TerminalGlyphAtlas> m_atlas;
    QImage m_mask;
    QImage m_colors;
    quint64 m_maskRevision = 0;
    quint64 m_colorRevision = 0;
};

} // namespace span::terminal
