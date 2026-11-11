#pragma once

#include <QString>
#include <QVector>

namespace span::workspace {

// One tile of the workspace canvas. The singleton "projects" tile fills the
// left zone; "terminal" and "editor" tiles occupy one cell of a center
// column, or both (rowSpan 2).
struct Tile {
    QString id;
    QString kind;
    int row = 0;
    int rowSpan = 2;
    int column = 0;
    QString sessionId;
    // Scope key of a terminal or editor tile (workspace/scope.h).
    QString projectPath;

    bool inCenter() const { return kind != QLatin1String("projects"); }
};

// Arithmetic of the center tile grid: columns of one full-height tile or two
// stacked tiles, their pixel widths and their row split. Pure functions over
// the values they are given; WorkspaceLayout owns the state.
namespace tile_grid {

// Where a dragged tile lands relative to the target column.
enum DropSlot { DropColumn = 0, DropUpper = 1, DropLower = 2, DropLeft = 3, DropRight = 4 };

// "terminal" and "editor" tiles belong to a scope.
bool isProjectBoundKind(const QString &kind);
int minWidthForKind(const QString &kind);
int columnCount(const QVector<Tile> &tiles);
// Indices of the center tiles in `column`.
QVector<int> columnOccupants(const QVector<Tile> &tiles, int column);
int columnMinWidth(const QVector<Tile> &tiles, int column);
// A column that lost a tile: a lone survivor becomes full height; an empty
// column is removed with its width and ratio. Returns true when removed.
bool vacateColumn(QVector<Tile> &tiles, QVector<int> &widths, QVector<qreal> &ratios, int column);
// New columns (width <= 0) take the mean of the existing ones, or
// `fallback` when none exist.
void fillNewColumnWidths(QVector<int> &widths, int fallback);
// Scales `weights` to `available` pixels, honouring per-column minimums.
QVector<int> distributeWidths(const QVector<int> &weights, const QVector<Tile> &tiles, int available);
// Sizes `widths` to the grid's columns, then distributes `available`.
void reconcileWidths(QVector<int> &widths, const QVector<Tile> &tiles, int available);
// Moves tile `id` to `slot` (a DropSlot) of `column`. Returns false when the
// move is illegal or changes nothing; the arguments are then unspecified, so
// callers pass copies.
bool moveTile(QVector<Tile> &tiles, QVector<int> &widths, QVector<qreal> &ratios, const QString &id, int column,
              int slot, int available);
// 0.5 when the center is too short for two minimum-height rows.
qreal clampedRowRatio(qreal ratio, int centerHeight);
// The DropSlot under a pointer at the given fractions of a column.
int dropSlotAt(qreal xRatio, qreal yRatio);

} // namespace tile_grid
} // namespace span::workspace
