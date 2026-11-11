#include "terminal/render/terminal_scene_graph.h"

#include "terminal/render/terminal_cell_material.h"
#include "terminal/render/terminal_drawn_glyphs.h"

#include <QMatrix4x4>
#include <QHash>
#include <QColor>
#include <QImage>
#include <QSGTexture>
#include <QTextLayout>
#include <QQuickWindow>
#include <QSGNode>
#include <QSGRendererInterface>
#include <QSGClipNode>
#include <QSGGeometryNode>
#include <QSGSimpleRectNode>
#include <QSGSimpleTextureNode>
#include <QSGTextNode>
#include <QSGTransformNode>
#include <QSet>
#include <QTextLine>
#include <QTextOption>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <memory>
#include <unordered_map>
#include <utility>
#include <vector>

namespace span::terminal {

namespace {

// Textures and material of one glyph atlas, shared by every row drawn with
// it; on the software renderer, its CPU rasterizer instead.
struct AtlasMaterial {
    std::unique_ptr<TerminalAtlasTexture> mask;
    std::unique_ptr<TerminalAtlasTexture> colors;
    std::unique_ptr<TerminalCellMaterial> material;
    std::unique_ptr<TerminalCellRasterizer> rasterizer;
};

class TerminalRowNode final : public QSGTransformNode {
public:
    TerminalRowNode() = default;
    ~TerminalRowNode() override { clear(); }

    void setRowPosition(qreal top, qreal devicePixelRatio)
    {
        const qreal snappedTop = TerminalGlyphGeometry::snap(top, devicePixelRatio);
        if (!nearlyEqual(m_top, snappedTop)) {
            m_top = snappedTop;
            QMatrix4x4 matrix;
            matrix.translate(0.0f, float(snappedTop));
            setMatrix(matrix);
        }
    }

    // Whether the node already shows this prepared row in this blink phase.
    bool presents(const TerminalPreparedRow *row, bool blinkHidden) const
    {
        return m_row.get() == row && m_blinkHidden == blinkHidden;
    }

    const TerminalPreparedRow *shownRow() const { return m_row.get(); }

    // A row not prepared yet draws nothing until it is.
    void showNothing()
    {
        if (!m_row) return;
        clear();
        m_row.reset();
    }

    // The whole row is one node; a new row only replaces its content, which
    // the renderer uploads once and then moves by matrix.
    void show(const std::shared_ptr<const TerminalPreparedRow> &source, bool blinkHidden, AtlasMaterial &atlas,
              QQuickWindow *window, qreal devicePixelRatio)
    {
        m_row = source;
        m_blinkHidden = blinkHidden;
        const QVector<TerminalCellVertex> &vertices =
            blinkHidden ? source->blinkHiddenVertices : source->geometry.vertices;
        const QVector<quint16> &indices = source->geometry.indices;
        if (vertices.isEmpty() || indices.isEmpty()) {
            clear();
        } else if (atlas.rasterizer) {
            showImage(vertices, indices, *atlas.rasterizer, window, devicePixelRatio);
        } else {
            showGeometry(vertices, indices, atlas.material.get());
        }
    }

private:
    static bool nearlyEqual(qreal left, qreal right)
    {
        return qAbs(left - right) <= 0.0001;
    }

    void showGeometry(const QVector<TerminalCellVertex> &vertices, const QVector<quint16> &indices,
                      TerminalCellMaterial *material)
    {
        const bool added = !m_geometry;
        if (added) {
            clear();
            auto *geometry = new QSGGeometry(TerminalCellMaterial::attributes(), 0, 0, QSGGeometry::UnsignedShortType);
            geometry->setDrawingMode(QSGGeometry::DrawTriangles);
            geometry->setVertexDataPattern(QSGGeometry::StaticPattern);
            geometry->setIndexDataPattern(QSGGeometry::StaticPattern);
            m_geometry = new QSGGeometryNode;
            m_geometry->setGeometry(geometry);
            m_geometry->setFlag(QSGNode::OwnsGeometry);
        }
        QSGGeometry *geometry = m_geometry->geometry();
        geometry->allocate(int(vertices.size()), int(indices.size()));
        std::memcpy(geometry->vertexData(), vertices.constData(), size_t(vertices.size()) * sizeof(TerminalCellVertex));
        std::memcpy(geometry->indexData(), indices.constData(), size_t(indices.size()) * sizeof(quint16));
        geometry->markVertexDataDirty();
        geometry->markIndexDataDirty();
        if (m_geometry->material() != material) m_geometry->setMaterial(material);
        // A renderer reads geometry and material as soon as the node is added.
        if (added) {
            appendChildNode(m_geometry);
        } else {
            m_geometry->markDirty(QSGNode::DirtyGeometry);
        }
    }

    // The software renderer draws no custom material: the row is painted on
    // the CPU once and drawn as a texture.
    void showImage(const QVector<TerminalCellVertex> &vertices, const QVector<quint16> &indices,
                   TerminalCellRasterizer &rasterizer, QQuickWindow *window, qreal devicePixelRatio)
    {
        QRectF bounds;
        const QImage image = rasterizer.paint(vertices, indices, devicePixelRatio, &bounds);
        std::unique_ptr<QSGTexture> texture(image.isNull() ? nullptr : window->createTextureFromImage(image));
        if (!texture) {
            clear();
            return;
        }
        const bool added = !m_image;
        if (added) {
            clear();
            m_image = new QSGSimpleTextureNode;
            m_image->setOwnsTexture(false);
            m_image->setFiltering(QSGTexture::Nearest);
        }
        m_image->setTexture(texture.get());
        m_image->setRect(bounds);
        if (added) appendChildNode(m_image);
        m_imageTexture = std::move(texture);
    }

    void clear()
    {
        if (m_geometry) {
            removeChildNode(m_geometry);
            delete m_geometry;
            m_geometry = nullptr;
        }
        if (m_image) {
            removeChildNode(m_image);
            delete m_image;
            m_image = nullptr;
        }
        m_imageTexture.reset();
    }

    std::shared_ptr<const TerminalPreparedRow> m_row;
    QSGGeometryNode *m_geometry = nullptr;
    QSGSimpleTextureNode *m_image = nullptr;
    std::unique_ptr<QSGTexture> m_imageTexture;
    bool m_blinkHidden = false;
    qreal m_top = -1.0;
};

// Clips to the grid; everything drawn sits in `content`, whose translation is
// the part of the top row scrolled out. Scrolling within a row moves only it.
class TerminalSceneNode final : public QSGClipNode {
public:
    explicit TerminalSceneNode(quint64 sceneGeneration) : sceneGeneration(sceneGeneration)
    {
        setIsRectangular(true);
        content = new QSGTransformNode;
        appendChildNode(content);
        graphicsLayer = new QSGClipNode;
        graphicsLayer->setIsRectangular(true);
        content->appendChildNode(graphicsLayer);
        searchLayer = new QSGNode;
        content->appendChildNode(searchLayer);
        selectionLayer = new QSGNode;
        content->appendChildNode(selectionLayer);
        cursor = new QSGSimpleRectNode;
        content->appendChildNode(cursor);
        preeditLayer = new QSGNode;
        content->appendChildNode(preeditLayer);
    }

    void setContentOffset(qreal y)
    {
        if (contentOffset == y) return;
        contentOffset = y;
        QMatrix4x4 matrix;
        matrix.translate(0.0f, float(y));
        content->setMatrix(matrix);
    }

    ~TerminalSceneNode() override
    {
        // Nodes must stop referring to shared textures before those textures die.
        for (auto *node : std::as_const(graphicNodes)) {
            graphicsLayer->removeChildNode(node);
            delete node;
        }
        for (const auto &texture : std::as_const(graphicTextures)) {
            delete texture.texture;
        }
        for (TerminalRowNode *row : std::as_const(rows)) {
            content->removeChildNode(row);
            delete row;
        }
    }

    AtlasMaterial &materialFor(const std::shared_ptr<const TerminalGlyphAtlas> &atlas,
                               QSGRendererInterface::GraphicsApi api)
    {
        auto found = atlasMaterials.find(atlas.get());
        if (found == atlasMaterials.end()) {
            AtlasMaterial entry;
            if (api == QSGRendererInterface::Software) {
                entry.rasterizer = std::make_unique<TerminalCellRasterizer>(atlas);
            } else {
                entry.mask = std::make_unique<TerminalAtlasTexture>(atlas, TerminalGlyphAtlas::Page::Mask);
                entry.colors = std::make_unique<TerminalAtlasTexture>(atlas, TerminalGlyphAtlas::Page::Color);
                entry.material = std::make_unique<TerminalCellMaterial>(entry.mask.get(), entry.colors.get(),
                                                                        TerminalCellMaterial::supportsSubpixel(api));
            }
            found = atlasMaterials.emplace(atlas.get(), std::move(entry)).first;
        }
        return found->second;
    }

    // Drops the textures of atlases no row shows any more.
    void pruneAtlasMaterials()
    {
        QSet<const TerminalGlyphAtlas *> shown;
        for (const TerminalRowNode *row : std::as_const(rows)) {
            if (const TerminalPreparedRow *source = row->shownRow()) shown.insert(source->geometry.atlas.get());
        }
        for (auto it = atlasMaterials.begin(); it != atlasMaterials.end();) {
            it = shown.contains(it->first) ? std::next(it) : atlasMaterials.erase(it);
        }
    }

    std::unordered_map<const TerminalGlyphAtlas *, AtlasMaterial> atlasMaterials;

    struct GraphicTexture {
        QSGTexture *texture = nullptr;
        qint64 imageKey = 0;
    };
    QHash<QString, QSGSimpleTextureNode *> graphicNodes;
    QHash<QString, GraphicTexture> graphicTextures;

    QVector<TerminalRowNode *> rows;
    QSGTransformNode *content = nullptr;
    qreal contentOffset = 0;
    QRectF gridClip;
    QSGClipNode *graphicsLayer = nullptr;
    QSGNode *searchLayer = nullptr;
    QSGNode *selectionLayer = nullptr;
    QSGSimpleRectNode *cursor = nullptr;
    QSGNode *preeditLayer = nullptr;
    quint64 graphicsRevision = 0;
    quint64 graphicSnapshotSequence = std::numeric_limits<quint64>::max();
    qreal graphicCellWidth = 0;
    qreal graphicCellHeight = 0;
    QRectF graphicViewport;
    int graphicLiveRow = 0;
    quint64 contentSequence = 0;
    quint64 viewportTraceSequence = std::numeric_limits<quint64>::max();
    quint64 viewportTraceEpoch = std::numeric_limits<quint64>::max();
    bool searchValid = false;
    QVector<TerminalHighlight> searchHighlights;
    QColor searchMatchColor;
    QColor searchCurrentColor;
    qreal searchCellWidth = 0;
    qreal searchCellHeight = 0;
    bool selectionValid = false;
    QVector<TerminalHighlight> selectionHighlights;
    QColor selectionColor;
    qreal selectionCellWidth = 0;
    qreal selectionCellHeight = 0;
    quint64 sceneGeneration = 0;
    std::vector<std::unique_ptr<QTextLayout>> preeditLayouts;
};

void clearChildren(QSGNode *node)
{
    while (node && node->firstChild()) {
        QSGNode *child = node->firstChild();
        node->removeChildNode(child);
        delete child;
    }
}

// One rectangle per highlight on a drawn row.
void paintHighlights(QSGNode *layer, const QVector<TerminalHighlight> &highlights, qsizetype rowCount, qreal cellWidth,
                     qreal cellHeight, const QColor &color, const QColor &currentColor)
{
    clearChildren(layer);
    for (const TerminalHighlight &mark : highlights) {
        if (mark.line < 0 || mark.line >= rowCount || mark.endColumn <= mark.startColumn) continue;
        layer->appendChildNode(
            new QSGSimpleRectNode(QRectF(mark.startColumn * cellWidth, mark.line * cellHeight,
                                         (mark.endColumn - mark.startColumn) * cellWidth, cellHeight),
                                  mark.current ? currentColor : color));
    }
}

} // namespace

QRectF TerminalSceneGraph::graphicRect(const GraphicPlacement &graphic, qreal cellWidth, qreal cellHeight)
{
    if (graphic.pixelSize.isEmpty() || cellWidth <= 0 || cellHeight <= 0) {
        return {};
    }
    const qreal scaleX = cellWidth / std::max(1, qRound(cellWidth));
    const qreal scaleY = cellHeight / std::max(1, qRound(cellHeight));
    return QRectF(graphic.column * cellWidth + graphic.cellOffset.x() * scaleX,
                  graphic.row * cellHeight + graphic.cellOffset.y() * scaleY, graphic.pixelSize.width() * scaleX,
                  graphic.pixelSize.height() * scaleY);
}

TerminalSceneGraph::PreeditPlacement TerminalSceneGraph::placePreedit(
    const TerminalImeCompositionState &composition,
    const QFont &font,
    qreal anchorX,
    qreal cellWidth,
    qreal viewportWidth)
{
    const TerminalImeLayoutMetrics metrics = composition.layoutMetrics(font);
    PreeditPlacement result;
    result.textWidth = std::max(cellWidth, metrics.textWidth);
    result.originX = std::max<qreal>(0.0, anchorX);
    const qreal caretWidth = std::max<qreal>(1.0, cellWidth * 0.12);

    if (result.textWidth <= viewportWidth) {
        result.originX = std::min(result.originX,
                                  std::max<qreal>(0.0, viewportWidth - result.textWidth));
    } else {
        const qreal desiredRight = std::max<qreal>(caretWidth, viewportWidth - caretWidth);
        if (result.originX + metrics.cursorX > desiredRight) {
            result.originX = desiredRight - metrics.cursorX;
        }
        result.originX = std::clamp(result.originX,
                                    viewportWidth - result.textWidth,
                                    qreal(0.0));
    }
    result.cursorX = std::clamp(result.originX + metrics.cursorX,
                                qreal(0.0),
                                std::max<qreal>(0.0, viewportWidth - caretWidth));
    return result;
}

QSGNode *TerminalSceneGraph::sync(QSGNode *oldNode, const PaintState &state, PaintResult &result)
{
    if (!state.frame || !state.rows || !state.imeComposition) {
        // Release builds must not dereference a partial paint state.
        return oldNode;
    }
    result = {};

    auto *root = static_cast<TerminalSceneNode *>(oldNode);
    const bool rebuildScene = !root
        || root->sceneGeneration != state.sceneGeneration;
    if (rebuildScene) {
        delete root;
        root = new TerminalSceneNode(state.sceneGeneration);
    }

    if (state.contentPresentationSequence != 0
        && root->contentSequence != state.contentPresentationSequence) {
        root->contentSequence = state.contentPresentationSequence;
        result.contentSequenceAdvanced = true;
    }

    const qreal dpr = state.devicePixelRatio;
    const qreal rowHeight = state.cellHeight;
    const QRectF gridClip(0, 0, state.itemWidth, state.frame->rows * rowHeight);
    if (root->gridClip != gridClip) {
        root->gridClip = gridClip;
        root->setClipRect(gridClip);
    }
    root->setContentOffset(TerminalGlyphGeometry::snap(state.rowOffset, dpr));

    // Scrolling hands prepared rows to other indices. Each row takes the node
    // that already shows it, so only rows with new content upload vertices;
    // nodes left over go to rows nobody shows, and only then are any dropped.
    const qsizetype rowCount = state.rows->size();
    QHash<const TerminalPreparedRow *, int> nodeByShownRow;
    nodeByShownRow.reserve(root->rows.size());
    for (int index = 0; index < root->rows.size(); ++index) {
        if (const TerminalPreparedRow *shown = root->rows.at(index)->shownRow()) nodeByShownRow.insert(shown, index);
    }
    QVector<TerminalRowNode *> ordered(rowCount, nullptr);
    QVector<bool> taken(root->rows.size(), false);
    for (int row = 0; row < rowCount; ++row) {
        const auto shown = nodeByShownRow.constFind(state.rows->at(row).get());
        if (shown != nodeByShownRow.cend() && !taken.at(*shown)) {
            ordered[row] = root->rows.at(*shown);
            taken[*shown] = true;
        }
    }
    int spare = 0;
    for (TerminalRowNode *&slot : ordered) {
        if (slot) continue;
        while (spare < taken.size() && taken.at(spare)) ++spare;
        if (spare < taken.size()) {
            slot = root->rows.at(spare);
            taken[spare] = true;
        } else {
            slot = new TerminalRowNode;
            root->content->insertChildNodeBefore(slot, root->cursor);
        }
    }
    for (int index = 0; index < taken.size(); ++index) {
        if (!taken.at(index)) {
            root->content->removeChildNode(root->rows.at(index));
            delete root->rows.at(index);
        }
    }
    root->rows = std::move(ordered);

    const QSGRendererInterface::GraphicsApi api =
        state.window ? state.window->rendererInterface()->graphicsApi() : QSGRendererInterface::Unknown;
    for (int row = 0; row < root->rows.size(); ++row) {
        auto *node = root->rows.at(row);
        node->setRowPosition(row * rowHeight, dpr);
        const std::shared_ptr<const TerminalPreparedRow> &source = state.rows->at(row);
        if (!source) {
            node->showNothing();
            continue;
        }
        // Unchanged rows keep their prepared pointer, so this is the whole
        // dirty test; blink only swaps between two prepared vertex sets.
        const bool blinkHidden = !state.blinkVisible && source->hasBlink;
        if (node->presents(source.get(), blinkHidden)) {
            continue;
        }
        node->show(source, blinkHidden, root->materialFor(source->geometry.atlas, api), state.window, dpr);
        ++result.updatedRows;
    }
    root->pruneAtlasMaterials();

    if (state.traceEnabled
        && (root->viewportTraceSequence != state.lastSnapshotSequence
            || root->viewportTraceEpoch != state.viewportEpoch)) {
        root->viewportTraceSequence = state.lastSnapshotSequence;
        root->viewportTraceEpoch = state.viewportEpoch;
        result.viewportTraceChanged = true;
        result.rowCount = root->rows.size();
        for (const auto *row : std::as_const(root->rows)) {
            if (row) {
                result.rowChildCount += row->childCount();
            }
        }
    }

    // Graphics rows are relative to the viewport; the clip spans the drawn
    // rows in content coordinates.
    const QRectF graphicViewport(0, 0, std::min(state.itemWidth, state.frame->columns * state.cellWidth),
                                 rowCount * state.cellHeight);
    if (state.window
        && (root->graphicsRevision != state.graphicsRevision
            || root->graphicSnapshotSequence != state.lastSnapshotSequence || root->graphicCellWidth != state.cellWidth
            || root->graphicCellHeight != state.cellHeight || root->graphicViewport != graphicViewport
            || root->graphicLiveRow != state.liveRow)) {
        root->graphicsLayer->setClipRect(graphicViewport);
        QSet<QString> liveNodes;
        QSet<QString> liveTextures;
        QVector<QSGTexture *> retiredTextures;
        QVector<const GraphicPlacement *> ordered;
        ordered.reserve(state.frame->graphics.size());
        for (const auto &graphic : state.frame->graphics) {
            ordered.append(&graphic);
        }
        std::stable_sort(ordered.begin(), ordered.end(), [](const auto *left, const auto *right) {
            return left->zIndex < right->zIndex;
        });
        QSGNode *previousNode = nullptr;
        for (const auto *placement : std::as_const(ordered)) {
            const auto &graphic = *placement;
            if (!graphic.resource || liveNodes.contains(graphic.id)) {
                continue;
            }
            const QImage &image = graphic.resource->image;
            if (image.isNull()) {
                continue;
            }
            const QString &resourceId = graphic.resource->id;
            liveTextures.insert(resourceId);
            const QRectF rect =
                graphicRect(graphic, state.cellWidth, state.cellHeight).translated(0, state.liveRow * state.cellHeight);
            if (!rect.intersects(graphicViewport)) {
                continue;
            }
            auto texture = root->graphicTextures.find(resourceId);
            if (texture == root->graphicTextures.end()
                || texture->imageKey != image.cacheKey()) {
                QSGTexture *uploaded = state.window->createTextureFromImage(image);
                if (!uploaded) {
                    continue;
                }
                if (texture != root->graphicTextures.end()) {
                    retiredTextures.append(texture->texture);
                }
                texture = root->graphicTextures.insert(resourceId, {uploaded, image.cacheKey()});
            }
            auto *node = root->graphicNodes.value(graphic.id);
            if (!node) {
                node = new QSGSimpleTextureNode;
                node->setOwnsTexture(false);
                node->setFiltering(QSGTexture::Linear);
                root->graphicNodes.insert(graphic.id, node);
            }
            if (node->texture() != texture->texture) {
                node->setTexture(texture->texture);
            }
            if (node->rect() != rect) {
                node->setRect(rect);
            }
            QSGNode *expected = previousNode
                ? previousNode->nextSibling() : root->graphicsLayer->firstChild();
            if (expected != node) {
                if (node->parent()) {
                    root->graphicsLayer->removeChildNode(node);
                }
                if (previousNode) {
                    root->graphicsLayer->insertChildNodeAfter(node, previousNode);
                } else {
                    root->graphicsLayer->prependChildNode(node);
                }
            }
            previousNode = node;
            liveNodes.insert(graphic.id);
        }
        for (auto it = root->graphicNodes.begin(); it != root->graphicNodes.end();) {
            if (!liveNodes.contains(it.key())) {
                root->graphicsLayer->removeChildNode(it.value());
                delete it.value();
                it = root->graphicNodes.erase(it);
            } else {
                ++it;
            }
        }
        for (auto it = root->graphicTextures.begin(); it != root->graphicTextures.end();) {
            if (!liveTextures.contains(it.key())) {
                delete it->texture;
                it = root->graphicTextures.erase(it);
            } else {
                ++it;
            }
        }
        qDeleteAll(retiredTextures);
        root->graphicsRevision = state.graphicsRevision;
        root->graphicSnapshotSequence = state.lastSnapshotSequence;
        root->graphicCellWidth = state.cellWidth;
        root->graphicCellHeight = state.cellHeight;
        root->graphicViewport = graphicViewport;
        root->graphicLiveRow = state.liveRow;
    }

    const bool searchUnchanged = root->searchValid
        && root->searchHighlights == state.searchHighlights
        && root->searchMatchColor == state.searchMatchColor
        && root->searchCurrentColor == state.searchCurrentColor
        && qFuzzyCompare(root->searchCellWidth, state.cellWidth)
        && qFuzzyCompare(root->searchCellHeight, state.cellHeight);
    if (!searchUnchanged) {
        paintHighlights(root->searchLayer, state.searchHighlights, rowCount, state.cellWidth, state.cellHeight,
                        state.searchMatchColor, state.searchCurrentColor);
        root->searchValid = true;
        root->searchHighlights = state.searchHighlights;
        root->searchMatchColor = state.searchMatchColor;
        root->searchCurrentColor = state.searchCurrentColor;
        root->searchCellWidth = state.cellWidth;
        root->searchCellHeight = state.cellHeight;
    }

    const bool selectionUnchanged = root->selectionValid && root->selectionHighlights == state.selectionHighlights
        && root->selectionColor == state.selectionColor && qFuzzyCompare(root->selectionCellWidth, state.cellWidth)
        && qFuzzyCompare(root->selectionCellHeight, state.cellHeight);
    if (!selectionUnchanged) {
        paintHighlights(root->selectionLayer, state.selectionHighlights, rowCount, state.cellWidth, state.cellHeight,
                        state.selectionColor, state.selectionColor);
        root->selectionValid = true;
        root->selectionHighlights = state.selectionHighlights;
        root->selectionColor = state.selectionColor;
        root->selectionCellWidth = state.cellWidth;
        root->selectionCellHeight = state.cellHeight;
    }

    const std::optional<Cursor> visualCursor = state.visualCursor;
    if (state.showCursor && visualCursor) {
        const Cursor &cursor = *visualCursor;
        QRectF rect(cursor.column * state.cellWidth, (state.liveRow + cursor.row) * state.cellHeight, state.cellWidth,
                    state.cellHeight);
        if (state.cursorShape == CursorShape::Underline) {
            rect.setY(rect.bottom() - std::max<qreal>(2.0, state.cellHeight * 0.12));
            rect.setHeight(std::max<qreal>(2.0, state.cellHeight * 0.12));
        } else if (state.cursorShape == CursorShape::Bar) {
            rect.setWidth(std::max<qreal>(2.0, state.cellWidth * 0.18));
        }
        root->cursor->setRect(rect);
        QColor animatedCursor = state.cursorColor;
        const qreal opacity = cursor.blink ? state.cursorOpacity : 1.0;
        if (opacity <= 0.001) {
            root->cursor->setRect(QRectF());
            root->cursor->setColor(Qt::transparent);
        } else {
            animatedCursor.setAlphaF(animatedCursor.alphaF() * opacity);
            root->cursor->setColor(animatedCursor);
        }
    } else {
        root->cursor->setRect(QRectF());
        root->cursor->setColor(Qt::transparent);
    }

    clearChildren(root->preeditLayer);
    root->preeditLayouts.clear();
    const TerminalImeCompositionState &ime = *state.imeComposition;
    const TerminalImeAnchor &imeAnchor = ime.anchor();
    if (state.paintPreedit && state.window && imeAnchor.isValid()
        && imeAnchor.cursor.row >= 0 && imeAnchor.cursor.row < state.frame->rows) {
        const Cursor &preeditCursor = imeAnchor.cursor;
        const qreal preeditDpr = std::max<qreal>(1.0, state.devicePixelRatio);
        const QFont preeditFont = state.preeditFont;
        const qreal viewportWidth = std::max<qreal>(
            1.0, std::min(state.itemWidth, state.frame->columns * state.cellWidth));
        const PreeditPlacement placement = placePreedit(
            ime, preeditFont,
            preeditCursor.column * state.cellWidth, state.cellWidth, viewportWidth);
        const qreal textWidth = TerminalGlyphGeometry::snap(
            std::min(placement.textWidth,
                     std::max<qreal>(0.0, viewportWidth - placement.originX)),
            preeditDpr);
        const qreal textHeight = state.cellHeight;
        const qreal originX = TerminalGlyphGeometry::snap(
            placement.originX, preeditDpr);
        const qreal originY = (state.liveRow + preeditCursor.row) * state.cellHeight;
        const qreal thickness = std::max<qreal>(1.0 / preeditDpr, textHeight * 0.08);

        QColor backdrop = state.background;
        if (backdrop.alpha() < 255) {
            backdrop = QColor(state.background.rgb());
            backdrop.setAlpha(255);
        }
        root->preeditLayer->appendChildNode(
            new QSGSimpleRectNode(QRectF(originX, originY, textWidth, textHeight), backdrop));

        QColor wash = state.cursorColor;
        wash.setAlpha(state.background.lightness() > 128 ? 36 : 56);
        root->preeditLayer->appendChildNode(
            new QSGSimpleRectNode(QRectF(originX, originY, textWidth, textHeight), wash));

        auto layout = std::make_unique<QTextLayout>(ime.preedit(), preeditFont);
        QTextOption option;
        option.setWrapMode(QTextOption::NoWrap);
        layout->setTextOption(option);
        layout->setFormats(ime.layoutFormats());
        layout->beginLayout();
        QTextLine line = layout->createLine();
        if (line.isValid()) {
            line.setLineWidth(std::numeric_limits<qreal>::max());
            line.setPosition(QPointF());
        }
        layout->endLayout();

        if (line.isValid()) {
            for (const TerminalImeTextFormat &range : ime.formats()) {
                if (range.format.background().style() == Qt::NoBrush) {
                    continue;
                }
                const qreal startX = line.cursorToX(range.start);
                const qreal endX = line.cursorToX(range.start + range.length);
                const qreal left = std::clamp(
                    originX + std::min(startX, endX), qreal(0.0), viewportWidth);
                const qreal right = std::clamp(
                    originX + std::max(startX, endX), qreal(0.0), viewportWidth);
                const QRectF formatRect(left, originY,
                                        std::max<qreal>(0.0, right - left),
                                        textHeight);
                if (!formatRect.isEmpty()) {
                    root->preeditLayer->appendChildNode(new QSGSimpleRectNode(
                        formatRect, range.format.background().color()));
                }
            }
        }
        root->preeditLayer->appendChildNode(
            new QSGSimpleRectNode(QRectF(originX,
                                        originY + textHeight - std::max<qreal>(thickness, textHeight * 0.12),
                                        textWidth,
                                        thickness),
                                 state.cursorColor));

        if (line.isValid()) {
            QSGTextNode *textNode = state.window->createTextNode();
            textNode->setRenderType(QSGTextNode::QtRendering);
            textNode->setFiltering(QSGTexture::Linear);
            textNode->setColor(state.foreground);
            textNode->addTextLayout(
                QPointF(originX, originY + state.baseline - line.ascent()),
                layout.get());
            root->preeditLayer->appendChildNode(textNode);
            root->preeditLayouts.push_back(std::move(layout));
        }

        if (ime.cursorVisible()) {
            QColor caretColor = ime.cursorColor();
            if (!caretColor.isValid()) {
                caretColor = state.cursorColor;
            }
            const qreal caretWidth = std::max<qreal>(1.0 / preeditDpr, state.cellWidth * 0.12);
            root->preeditLayer->appendChildNode(new QSGSimpleRectNode(
                QRectF(placement.cursorX, originY, caretWidth, textHeight),
                caretColor));
        }
    }
    return root;
}

} // namespace span::terminal
