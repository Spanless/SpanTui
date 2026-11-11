#include "workspace/tile_grid.h"

#include "workspace/workspace_constants.h"

#include <algorithm>
#include <cmath>

namespace span::workspace::tile_grid {
namespace {

constexpr int kMinColumnWidth = 250;
constexpr int kMinTerminalWidth = 440;
constexpr int kMinEditorWidth = 320;
// A pointer left of the first fraction of a column, or right of the second,
// targets a new column on that side.
constexpr qreal kLeftDropBelow = 0.18;
constexpr qreal kRightDropAbove = 0.82;

} // namespace

bool isProjectBoundKind(const QString &kind)
{
    return kind == QLatin1String("terminal") || kind == QLatin1String("editor");
}

int minWidthForKind(const QString &kind)
{
    if (kind == QLatin1String("terminal")) {
        return kMinTerminalWidth;
    }
    if (kind == QLatin1String("editor")) {
        return kMinEditorWidth;
    }
    return kMinColumnWidth;
}

int columnCount(const QVector<Tile> &tiles)
{
    int count = 0;
    for (const Tile &tile : tiles) {
        if (tile.inCenter() && tile.column >= 0) {
            count = std::max(count, tile.column + 1);
        }
    }
    return count;
}

QVector<int> columnOccupants(const QVector<Tile> &tiles, int column)
{
    QVector<int> rows;
    for (int i = 0; i < tiles.size(); ++i) {
        if (tiles.at(i).inCenter() && tiles.at(i).column == column) {
            rows.append(i);
        }
    }
    return rows;
}

int columnMinWidth(const QVector<Tile> &tiles, int column)
{
    int minimum = kMinColumnWidth;
    for (int i : columnOccupants(tiles, column)) {
        minimum = std::max(minimum, minWidthForKind(tiles.at(i).kind));
    }
    return minimum;
}

bool vacateColumn(QVector<Tile> &tiles, QVector<int> &widths, QVector<qreal> &ratios, int column)
{
    const QVector<int> rows = columnOccupants(tiles, column);
    if (rows.size() == 1) {
        Tile &tile = tiles[rows.constFirst()];
        tile.row = 0;
        tile.rowSpan = 2;
        return false;
    }
    if (!rows.isEmpty()) {
        return false;
    }
    for (Tile &tile : tiles) {
        if (tile.inCenter() && tile.column > column) {
            --tile.column;
        }
    }
    if (column >= 0 && column < widths.size()) {
        widths.removeAt(column);
    }
    if (column >= 0 && column < ratios.size()) {
        ratios.removeAt(column);
    }
    return true;
}

void fillNewColumnWidths(QVector<int> &widths, int fallback)
{
    int existingSum = 0;
    int existingCount = 0;
    for (int width : widths) {
        if (width > 0) {
            existingSum += width;
            ++existingCount;
        }
    }
    const int addedWeight = existingCount > 0 ? std::max(1, existingSum / existingCount) : fallback;
    for (int &width : widths) {
        if (width <= 0) {
            width = addedWeight;
        }
    }
}

QVector<int> distributeWidths(const QVector<int> &weights, const QVector<Tile> &tiles, int available)
{
    const int count = weights.size();
    available = std::max(0, available);
    QVector<int> mins(count);
    int minSum = 0;
    for (int column = 0; column < count; ++column) {
        mins[column] = columnMinWidth(tiles, column);
        minSum += mins.at(column);
    }
    if (available <= minSum) {
        return mins;
    }
    qreal weightSum = 0;
    for (int weight : weights) {
        weightSum += std::max(1, weight);
    }
    QVector<int> next(count);
    int assigned = 0;
    for (int column = 0; column < count; ++column) {
        next[column] =
            std::max(mins.at(column), int(std::floor(std::max(1, weights.at(column)) / weightSum * available)));
        assigned += next.at(column);
    }
    int delta = available - assigned;
    if (delta > 0) {
        next[count - 1] += delta;
    }
    for (int column = count - 1; column >= 0 && delta < 0; --column) {
        const int take = std::min(next.at(column) - mins.at(column), -delta);
        next[column] -= take;
        delta += take;
    }
    return next;
}

void reconcileWidths(QVector<int> &widths, const QVector<Tile> &tiles, int available)
{
    const int count = columnCount(tiles);
    if (count <= 0) {
        widths.clear();
        return;
    }
    widths.resize(count);
    fillNewColumnWidths(widths, kMinCenterWidth);
    widths = distributeWidths(widths, tiles, available);
}

bool moveTile(QVector<Tile> &tiles, QVector<int> &widths, QVector<qreal> &ratios, const QString &id, int column,
              int slot, int available)
{
    const auto fromIt = std::find_if(tiles.begin(), tiles.end(), [&id](const Tile &tile) { return tile.id == id; });
    if (fromIt == tiles.end() || !fromIt->inCenter()) {
        return false;
    }
    const int from = int(fromIt - tiles.begin());
    // Snapshot before taking `moving`: the non-const access below detaches
    // `tiles`, so the reference never aliases the snapshot's shared buffer.
    const QVector<Tile> originalTiles = tiles;
    Tile &moving = tiles[from];

    const auto shiftFrom = [&tiles](int fromCol, int delta) {
        for (Tile &tile : tiles) {
            if (tile.inCenter() && tile.column >= fromCol) {
                tile.column += delta;
            }
        }
    };
    const auto insertSlot = [&widths, &ratios](int at) {
        widths.insert(std::min(at, int(widths.size())), kMinCenterWidth);
        ratios.insert(std::min(at, int(ratios.size())), 0.5);
    };
    const auto placeFull = [&moving](int col) {
        moving.column = col;
        moving.row = 0;
        moving.rowSpan = 2;
    };

    int dest = std::clamp(column, 0, columnCount(tiles));

    // A full-height tile dropped anywhere on its own column is already where
    // it would land; removing it first would collapse or shift its neighbours.
    if (moving.column == dest && columnOccupants(tiles, dest).size() == 1) {
        return false;
    }

    // Step 1: remove the moving tile from its source column.
    const int sourceCol = moving.column;
    moving.column = -1;
    if (vacateColumn(tiles, widths, ratios, sourceCol) && sourceCol < dest) {
        --dest;
    }

    const int columns = columnCount(tiles);
    dest = std::min(dest, columns);

    // Step 2: insert into the target column and slot.
    if (slot == DropLeft) {
        shiftFrom(dest, 1);
        insertSlot(dest);
        placeFull(dest);
    } else if (slot == DropRight || slot == DropColumn || dest >= columns) {
        const int targetCol = (slot == DropRight && dest < columns) ? dest + 1 : dest;
        shiftFrom(targetCol, 1);
        insertSlot(targetCol);
        placeFull(targetCol);
    } else {
        const QVector<int> occ = columnOccupants(tiles, dest);
        if (occ.isEmpty()) {
            placeFull(dest);
        } else if (occ.size() == 1) {
            Tile &other = tiles[occ.constFirst()];
            other.rowSpan = 1;
            other.row = slot == DropUpper ? 1 : 0;
            moving.column = dest;
            moving.row = slot == DropUpper ? 0 : 1;
            moving.rowSpan = 1;
        } else {
            // A full stack: the tile in the target slot moves to its own new
            // column right of the destination.
            const int targetRow = (slot == DropUpper) ? 0 : 1;
            const auto victim = std::find_if(occ.cbegin(), occ.cend(), [&](int i) {
                return tiles.at(i).row == targetRow || tiles.at(i).rowSpan == 2;
            });
            if (victim != occ.cend()) {
                shiftFrom(dest + 1, 1);
                insertSlot(dest + 1);
                tiles[*victim].column = dest + 1;
                tiles[*victim].row = 0;
                tiles[*victim].rowSpan = 2;
            }
            moving.column = dest;
            moving.row = targetRow;
            moving.rowSpan = 1;
        }
    }

    // Re-inserting a tile at its own slot is a no-op: keep the widths the
    // removal/insertion round trip would otherwise reset.
    const auto samePlacement = [](const Tile &a, const Tile &b) {
        return a.id == b.id && a.column == b.column && a.row == b.row && a.rowSpan == b.rowSpan;
    };
    if (std::equal(tiles.cbegin(), tiles.cend(), originalTiles.cbegin(), originalTiles.cend(), samePlacement)) {
        return false;
    }

    reconcileWidths(widths, tiles, available);
    return true;
}

qreal clampedRowRatio(qreal ratio, int centerHeight)
{
    if (centerHeight < kMinRowHeight * 2) {
        return 0.5;
    }
    const qreal minRatio = qreal(kMinRowHeight) / qreal(centerHeight);
    return std::clamp(ratio, minRatio, 1.0 - minRatio);
}

int dropSlotAt(qreal xRatio, qreal yRatio)
{
    if (std::isfinite(xRatio)) {
        if (xRatio < kLeftDropBelow) {
            return DropLeft;
        }
        if (xRatio > kRightDropAbove) {
            return DropRight;
        }
    }
    if (!std::isfinite(yRatio) || yRatio >= 0.5) {
        return DropLower;
    }
    return DropUpper;
}

} // namespace span::workspace::tile_grid
