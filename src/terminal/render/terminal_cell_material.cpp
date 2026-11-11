#include "terminal/render/terminal_cell_material.h"

#include "terminal/render/terminal_row_geometry.h"
#include "terminal/render/terminal_triangle_raster.h"

#include <QMatrix4x4>
#include <QSGMaterialShader>
#include <QVarLengthArray>
#include <rhi/qrhi.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <iterator>
#include <limits>
#include <utility>

namespace span::terminal {

namespace {

// Qt Quick's QT_DF_BASE, QT_DF_BASEDEVIATION, QT_DF_SCALEFORMAXDEV,
// QT_DF_SCALEFORNODEV and QT_DF_RANGE defaults.
constexpr float kFieldBase = 0.5f;
constexpr float kFieldDeviation = 0.065f;
constexpr float kFieldFullDeviationScale = 0.15f;
constexpr float kFieldNoDeviationScale = 0.3f;
constexpr float kFieldRange = 0.06f;

} // namespace

std::pair<float, float> TerminalFieldText::alphaRange(float scale)
{
    const float lift = (std::clamp(scale, kFieldFullDeviationScale, kFieldNoDeviationScale) - kFieldFullDeviationScale)
        / (kFieldNoDeviationScale - kFieldFullDeviationScale);
    const float threshold = kFieldBase - kFieldDeviation * (1.0f - lift);
    const float spread = kFieldRange / scale;
    return {std::max(0.0f, threshold - spread), std::min(threshold + spread, 1.0f)};
}

float TerminalFieldText::coverage(float distance, float scale)
{
    const auto [low, high] = alphaRange(scale);
    if (distance <= low) return 0.0f;
    if (distance >= high) return 1.0f;
    const float t = (distance - low) / (high - low);
    return t * t * (3.0f - 2.0f * t);
}

namespace {

class TerminalCellShader final : public QSGMaterialShader {
public:
    explicit TerminalCellShader(bool subpixel)
    {
        setShaderFileName(VertexStage, QStringLiteral(":/shaders/terminal/terminal_cell.vert.qsb"));
        setShaderFileName(FragmentStage,
                          subpixel ? QStringLiteral(":/shaders/terminal/terminal_cell_subpixel.frag.qsb")
                                   : QStringLiteral(":/shaders/terminal/terminal_cell.frag.qsb"));
        if (subpixel) setFlag(UpdatesGraphicsPipelineState);
    }

    bool updateUniformData(RenderState &state, QSGMaterial *newMaterial, QSGMaterial *) override
    {
        // std140 block: mat4 modelViewMatrix, mat4 projectionMatrix, float
        // qt_Opacity, float dpr, float maskSize. The shader snaps rows to
        // device pixels between the two matrices, so they travel apart.
        QByteArray *buffer = state.uniformData();
        bool changed = false;
        if (state.isMatrixDirty()) {
            std::memcpy(buffer->data(), state.modelViewMatrix().constData(), 64);
            std::memcpy(buffer->data() + 64, state.projectionMatrix().constData(), 64);
            changed = true;
        }
        if (state.isOpacityDirty()) {
            const float opacity = state.opacity();
            std::memcpy(buffer->data() + 128, &opacity, sizeof(opacity));
            changed = true;
        }
        const TerminalGlyphAtlas *atlas = static_cast<TerminalCellMaterial *>(newMaterial)->mask()->atlas();
        const float constants[] = {float(state.devicePixelRatio()), float(atlas->size(TerminalGlyphAtlas::Page::Mask))};
        if (std::memcmp(buffer->data() + 132, constants, sizeof(constants)) != 0) {
            std::memcpy(buffer->data() + 132, constants, sizeof(constants));
            changed = true;
        }
        return changed;
    }

    bool updateGraphicsPipelineState(RenderState &, GraphicsPipelineState *state, QSGMaterial *, QSGMaterial *) override
    {
        // Each channel keeps what is behind it by its own weight.
        state->blendEnable = true;
        state->srcColor = GraphicsPipelineState::One;
        state->dstColor = GraphicsPipelineState::OneMinusSrc1Color;
        state->separateBlendFactors = true;
        state->srcAlpha = GraphicsPipelineState::One;
        state->dstAlpha = GraphicsPipelineState::OneMinusSrc1Alpha;
        return true;
    }

private:
    void updateSampledImage(RenderState &state, int binding, QSGTexture **texture, QSGMaterial *newMaterial,
                            QSGMaterial *) override
    {
        const auto *material = static_cast<TerminalCellMaterial *>(newMaterial);
        TerminalAtlasTexture *page = binding == 1 ? material->mask() : binding == 2 ? material->colors() : nullptr;
        if (!page) return;
        // Glyphs placed since the last frame reach the texture before the
        // rows that use them are drawn.
        page->commitTextureOperations(state.rhi(), state.resourceUpdateBatch());
        *texture = page;
    }
};

} // namespace

TerminalAtlasTexture::TerminalAtlasTexture(std::shared_ptr<const TerminalGlyphAtlas> atlas,
                                           TerminalGlyphAtlas::Page page)
    : m_atlas(std::move(atlas)),
      m_page(page)
{
    // Distance fields are scaled onto their quads. Bitmap quads cover whole
    // device pixels and sample texel centres, which filtering leaves exact.
    setFiltering(QSGTexture::Linear);
    setMipmapFiltering(QSGTexture::None);
    setHorizontalWrapMode(QSGTexture::ClampToEdge);
    setVerticalWrapMode(QSGTexture::ClampToEdge);
}

TerminalAtlasTexture::~TerminalAtlasTexture()
{
    if (m_texture) m_texture->deleteLater();
}

qint64 TerminalAtlasTexture::comparisonKey() const
{
    return qint64(quintptr(this));
}

QRhiTexture *TerminalAtlasTexture::rhiTexture() const
{
    return m_texture;
}

QSize TerminalAtlasTexture::textureSize() const
{
    return QSize(m_atlas->size(m_page), m_atlas->size(m_page));
}

void TerminalAtlasTexture::commitTextureOperations(QRhi *rhi, QRhiResourceUpdateBatch *resourceUpdates)
{
    if (!m_texture) {
        const bool mask = m_page == TerminalGlyphAtlas::Page::Mask;
        m_texture = rhi->newTexture(mask ? QRhiTexture::R8 : QRhiTexture::RGBA8, textureSize());
        if (!m_texture->create()) {
            delete m_texture;
            m_texture = nullptr;
            return;
        }
        m_uploaded = 0;
    }
    const QVector<TerminalGlyphAtlas::Upload> uploads = m_atlas->takeUploads(m_page, m_uploaded);
    if (uploads.isEmpty()) return;
    QVarLengthArray<QRhiTextureUploadEntry, 16> entries;
    entries.reserve(uploads.size());
    for (const TerminalGlyphAtlas::Upload &upload : uploads) {
        QRhiTextureSubresourceUploadDescription description(upload.image);
        description.setDestinationTopLeft(upload.position);
        entries.push_back(QRhiTextureUploadEntry(0, 0, description));
    }
    QRhiTextureUploadDescription description;
    description.setEntries(entries.cbegin(), entries.cend());
    resourceUpdates->uploadTexture(m_texture, description);
}

TerminalCellMaterial::TerminalCellMaterial(TerminalAtlasTexture *mask, TerminalAtlasTexture *colors, bool subpixel)
    : m_mask(mask),
      m_colors(colors),
      m_subpixel(subpixel)
{
    setFlag(QSGMaterial::Blending | QSGMaterial::RequiresFullMatrix);
}

bool TerminalCellMaterial::supportsSubpixel(QSGRendererInterface::GraphicsApi api)
{
    // Its shader is built for Direct3D only, where dual-source blending is
    // always there.
    return api == QSGRendererInterface::Direct3D11 || api == QSGRendererInterface::Direct3D12;
}

const QSGGeometry::AttributeSet &TerminalCellMaterial::attributes()
{
    static const QSGGeometry::Attribute data[] = {
        QSGGeometry::Attribute::createWithAttributeType(0, 2, QSGGeometry::FloatType, QSGGeometry::PositionAttribute),
        QSGGeometry::Attribute::createWithAttributeType(1, 2, QSGGeometry::FloatType, QSGGeometry::TexCoordAttribute),
        QSGGeometry::Attribute::createWithAttributeType(2, 4, QSGGeometry::UnsignedByteType,
                                                        QSGGeometry::ColorAttribute),
        QSGGeometry::Attribute::createWithAttributeType(3, 4, QSGGeometry::UnsignedByteType,
                                                        QSGGeometry::UnknownAttribute),
    };
    static const QSGGeometry::AttributeSet set = {4, int(sizeof(TerminalCellVertex)), data};
    return set;
}

QSGMaterialType *TerminalCellMaterial::type() const
{
    static QSGMaterialType greyscale;
    static QSGMaterialType subpixel;
    return m_subpixel ? &subpixel : &greyscale;
}

QSGMaterialShader *TerminalCellMaterial::createShader(QSGRendererInterface::RenderMode) const
{
    return new TerminalCellShader(m_subpixel);
}

int TerminalCellMaterial::compare(const QSGMaterial *other) const
{
    // The two pages of one atlas always go together.
    const qint64 left = m_mask->comparisonKey();
    const qint64 right = static_cast<const TerminalCellMaterial *>(other)->m_mask->comparisonKey();
    return left == right ? 0 : left < right ? -1 : 1;
}

TerminalCellRasterizer::TerminalCellRasterizer(std::shared_ptr<const TerminalGlyphAtlas> atlas)
    : m_atlas(std::move(atlas))
{
    using Page = TerminalGlyphAtlas::Page;
    m_mask = QImage(m_atlas->size(Page::Mask), m_atlas->size(Page::Mask), QImage::Format_Alpha8);
    m_mask.fill(0);
    m_colors = QImage(m_atlas->size(Page::Color), m_atlas->size(Page::Color), QImage::Format_RGBA8888_Premultiplied);
    m_colors.fill(Qt::transparent);
}

QImage TerminalCellRasterizer::paint(const QVector<TerminalCellVertex> &vertices, const QVector<quint16> &indices,
                                     qreal devicePixelRatio, QRectF *bounds)
{
    const auto mirror = [this](TerminalGlyphAtlas::Page page, QImage &texels, quint64 &revision) {
        const int depth = texels.depth() / 8;
        for (const TerminalGlyphAtlas::Upload &upload : m_atlas->takeUploads(page, revision)) {
            for (int y = 0; y < upload.image.height(); ++y) {
                std::memcpy(texels.scanLine(upload.position.y() + y) + upload.position.x() * depth,
                            upload.image.constScanLine(y), size_t(upload.image.width()) * depth);
            }
        }
    };
    mirror(TerminalGlyphAtlas::Page::Mask, m_mask, m_maskRevision);
    mirror(TerminalGlyphAtlas::Page::Color, m_colors, m_colorRevision);

    const float dpr = float(devicePixelRatio);
    float left = std::numeric_limits<float>::max();
    float top = left;
    float right = std::numeric_limits<float>::lowest();
    float bottom = right;
    for (const TerminalCellVertex &vertex : vertices) {
        left = std::min(left, vertex.x * dpr);
        top = std::min(top, vertex.y * dpr);
        right = std::max(right, vertex.x * dpr);
        bottom = std::max(bottom, vertex.y * dpr);
    }
    if (!(right > left && bottom > top)) return {};
    const int originX = int(std::floor(left));
    const int originY = int(std::floor(top));
    const int width = int(std::ceil(right)) - originX;
    const int height = int(std::ceil(bottom)) - originY;
    QImage image(width, height, QImage::Format_RGBA8888_Premultiplied);
    image.fill(Qt::transparent);

    const int texels = m_mask.width();
    const auto maskTexel = [this, texels](int tx, int ty) {
        return m_mask.constScanLine(std::clamp(ty, 0, texels - 1))[std::clamp(tx, 0, texels - 1)] / 255.0f;
    };
    // A field's distance between texel centres, filtered as on the GPU.
    const auto distance = [texels, &maskTexel](float u, float v) {
        const float x = u * texels - 0.5f;
        const float y = v * texels - 0.5f;
        const int left = int(std::floor(x));
        const int top = int(std::floor(y));
        const float fx = x - left;
        const float fy = y - top;
        return (maskTexel(left, top) * (1 - fx) + maskTexel(left + 1, top) * fx) * (1 - fy)
            + (maskTexel(left, top + 1) * (1 - fx) + maskTexel(left + 1, top + 1) * fx) * fy;
    };
    const int colorTexels = m_colors.width();
    for (qsizetype index = 0; index + 2 < indices.size(); index += 3) {
        const TerminalCellVertex *corner[3] = {&vertices.at(indices.at(index)), &vertices.at(indices.at(index + 1)),
                                               &vertices.at(indices.at(index + 2))};
        const bool field = corner[0]->field != 0;
        const bool color = corner[0]->color != 0;
        float x[3];
        float y[3];
        for (int k = 0; k < 3; ++k) {
            x[k] = corner[k]->x * dpr - originX;
            y[k] = corner[k]->y * dpr - originY;
        }
        // Device pixels a field texel spans, from how far u moves a pixel.
        float fieldScale = 1;
        if (field) {
            const float area = (x[1] - x[0]) * (y[2] - y[0]) - (x[2] - x[0]) * (y[1] - y[0]);
            const float du =
                ((corner[1]->u - corner[0]->u) * (y[2] - y[0]) - (corner[2]->u - corner[0]->u) * (y[1] - y[0])) / area;
            fieldScale = 1.0f / std::abs(du * texels);
        }
        rasterizeTriangle(x, y, width, height, [&](int px, int py, const float (&weights)[3]) {
            float u = 0;
            float v = 0;
            float ink[4] = {0, 0, 0, 0};
            for (int k = 0; k < 3; ++k) {
                u += weights[k] * corner[k]->u;
                v += weights[k] * corner[k]->v;
                ink[0] += weights[k] * corner[k]->r;
                ink[1] += weights[k] * corner[k]->g;
                ink[2] += weights[k] * corner[k]->b;
                ink[3] += weights[k] * corner[k]->a;
            }
            // A field becomes greyscale coverage, as the shader turns it;
            // then everything is texel times ink.
            float scale[4];
            if (color) {
                const int tx = std::clamp(int(u * colorTexels), 0, colorTexels - 1);
                const int ty = std::clamp(int(v * colorTexels), 0, colorTexels - 1);
                const uchar *texel = m_colors.constScanLine(ty) + tx * 4;
                for (int channel = 0; channel < 4; ++channel) scale[channel] = texel[channel] / 255.0f;
            } else {
                const float coverage = field ? TerminalFieldText::coverage(distance(u, v), fieldScale)
                                             : maskTexel(int(u * texels), int(v * texels));
                std::fill(std::begin(scale), std::end(scale), coverage);
            }
            const float sourceAlpha = scale[3] * ink[3];
            if (sourceAlpha <= 0) return;
            uchar *pixel = image.scanLine(py) + px * 4;
            for (int channel = 0; channel < 4; ++channel) {
                const float source = scale[channel] * ink[channel];
                const float blended = source + pixel[channel] * (255.0f - sourceAlpha) / 255.0f;
                pixel[channel] = uchar(std::clamp(std::lround(blended), 0L, 255L));
            }
        });
    }
    *bounds = QRectF(originX / devicePixelRatio, originY / devicePixelRatio, width / devicePixelRatio,
                     height / devicePixelRatio);
    return image;
}

} // namespace span::terminal
