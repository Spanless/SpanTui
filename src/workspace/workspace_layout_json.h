#pragma once

#include "workspace/tile_grid.h"
#include "workspace/workspace_constants.h"

#include <QString>
#include <QVector>

namespace span::workspace {

// Sizes and side-panel state of the workspace canvas.
struct LayoutGeometry {
    QVector<int> columnWidths;
    QVector<qreal> rowRatios;
    int leftWidthStored = kMinLeftWidth;
    bool leftCollapsed = false;
    int rightWidthStored = kDefaultRightWidth;
    bool rightCollapsed = true;
    QString rightPage = QStringLiteral("files");
};

// What the settings key workspace.layoutJson stores.
struct LayoutSnapshot {
    QVector<Tile> tiles;
    LayoutGeometry geometry;
    QString focusedTileId;
};

enum class LayoutParse {
    // `snapshot` holds a structurally valid layout: unique tile ids, legal
    // kinds, no overlapping cells, no empty columns, one width and one ratio
    // per column.
    Restored,
    // Nothing stored, or a version this build does not read.
    UseDefault,
    Invalid,
};

QString layoutToJson(const LayoutSnapshot &snapshot);
// Side widths, the right page and row ratios are returned as stored; the
// caller clamps them to its window.
LayoutParse layoutFromJson(const QString &json, LayoutSnapshot *snapshot);

} // namespace span::workspace
