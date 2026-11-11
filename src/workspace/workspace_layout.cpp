#include "workspace/workspace_layout.h"

#include <algorithm>
#include <cmath>

namespace span {
namespace workspace {
namespace {

const QString kZoneLeft = QStringLiteral("left");
const QString kZoneCenter = QStringLiteral("center");
const QString kZoneRight = QStringLiteral("right");
const QString kProjectsKind = QStringLiteral("projects");

bool nearlyEqual(qreal a, qreal b)
{
    return std::abs(a - b) <= 0.0001;
}

template <typename T>
QVariantList toVariantList(const QVector<T> &values)
{
    QVariantList list;
    list.reserve(values.size());
    for (const T &value : values) {
        list.append(value);
    }
    return list;
}

} // namespace

WorkspaceLayout::WorkspaceLayout(QObject *parent)
    : QAbstractListModel(parent)
{
    resetToDefault();
}

int WorkspaceLayout::rowCount(const QModelIndex &parent) const
{
    if (parent.isValid()) {
        return 0;
    }
    return m_tiles.size();
}

QVariant WorkspaceLayout::data(const QModelIndex &index, int role) const
{
    if (!index.isValid() || index.row() < 0 || index.row() >= m_tiles.size()) {
        return {};
    }
    const Tile &tile = m_tiles.at(index.row());
    switch (role) {
    case IdRole:
        return tile.id;
    case KindRole:
        return tile.kind;
    case ZoneRole:
        return tile.inCenter() ? kZoneCenter : kZoneLeft;
    case RowRole:
        return tile.row;
    case RowSpanRole:
        return tile.rowSpan;
    case ColumnRole:
        return tile.column;
    case SessionIdRole:
        return tile.sessionId;
    case ProjectPathRole:
        return tile.projectPath;
    default:
        return {};
    }
}

QHash<int, QByteArray> WorkspaceLayout::roleNames() const
{
    return {
        {IdRole, "id"},
        {KindRole, "kind"},
        {ZoneRole, "zone"},
        {RowRole, "row"},
        {RowSpanRole, "rowSpan"},
        {ColumnRole, "column"},
        {SessionIdRole, "sessionId"},
        {ProjectPathRole, "projectPath"},
    };
}

int WorkspaceLayout::leftWidth() const
{
    return !hasProjectsTile() || m_leftCollapsed ? 0 : m_leftWidthStored;
}

QVariantList WorkspaceLayout::centerRowRatios() const
{
    return toVariantList(m_rowRatios);
}

qreal WorkspaceLayout::centerRowRatio(int column) const
{
    if (column < 0 || column >= m_rowRatios.size()) {
        return 0.5;
    }
    return m_rowRatios.at(column);
}

QVariantList WorkspaceLayout::centerColumnWidths() const
{
    return toVariantList(m_columnWidths);
}

QString WorkspaceLayout::showProjectsTile()
{
    for (const Tile &tile : m_tiles) {
        if (!tile.inCenter()) {
            setFocused(tile.id);
            return tile.id;
        }
    }
    Tile tile;
    tile.id = allocateId();
    tile.kind = kProjectsKind;
    return insertNewTile(tile);
}

QString WorkspaceLayout::addProjectTile(const QString &kind,
                                        const QString &projectPath)
{
    if (!tile_grid::isProjectBoundKind(kind) || projectPath.trimmed().isEmpty()) {
        return {};
    }
    Tile tile;
    tile.id = allocateId();
    tile.kind = kind;
    tile.projectPath = projectPath;
    placeInCenter(tile);
    return insertNewTile(tile);
}

int WorkspaceLayout::dropSlotAt(qreal xRatio, qreal yRatio) const
{
    return tile_grid::dropSlotAt(xRatio, yRatio);
}

bool WorkspaceLayout::insertCenterTile(const QString &id, int column, int slot)
{
    QVector<Tile> tiles = m_tiles;
    QVector<int> widths = m_columnWidths;
    QVector<qreal> ratios = m_rowRatios;
    if (!tile_grid::moveTile(tiles, widths, ratios, id, column, slot, centerPixelWidth())) {
        return false;
    }
    m_tiles = tiles;
    m_columnWidths = widths;
    m_rowRatios = ratios;
    ensureRowRatioCount();
    notifyAllTilesChanged();
    emit geometryChanged();
    setFocused(id);
    return true;
}

QVariantMap WorkspaceLayout::previewCenterInsert(const QString &id, int column, int slot) const
{
    QVector<Tile> tiles = m_tiles;
    QVector<int> widths = m_columnWidths;
    QVector<qreal> ratios = m_rowRatios;
    if (!tile_grid::moveTile(tiles, widths, ratios, id, column, slot, centerPixelWidth())) {
        return {};
    }
    const auto found = std::find_if(tiles.cbegin(), tiles.cend(),
                                    [&id](const Tile &tile) { return tile.id == id; });
    if (found == tiles.cend()) {
        return {};
    }
    const qreal ratio = (found->column >= 0 && found->column < ratios.size())
                            ? ratios.at(found->column)
                            : 0.5;
    return {
        {QStringLiteral("ok"), true},
        {QStringLiteral("column"), found->column},
        {QStringLiteral("row"), found->row},
        {QStringLiteral("rowSpan"), found->rowSpan},
        {QStringLiteral("columnWidths"), toVariantList(widths)},
        {QStringLiteral("rowRatio"), ratio},
    };
}

bool WorkspaceLayout::closeTile(const QString &id)
{
    const int row = indexOfId(id);
    if (row < 0) {
        return false;
    }
    const Tile removed = m_tiles.at(row);
    beginRemoveRows(QModelIndex(), row, row);
    m_tiles.removeAt(row);
    endRemoveRows();
    if (removed.inCenter()) {
        compactCenterColumn(removed.column);
    }
    if (m_focusedTileId == id) {
        setFocused(m_tiles.isEmpty() ? QString() : m_tiles.constLast().id);
    }
    fillCenterColumns();
    notifyAllTilesChanged();
    emit geometryChanged();
    return true;
}

bool WorkspaceLayout::setLeftWidth(int px, int windowInnerWidth)
{
    return setSideWidth(px, windowInnerWidth, m_leftWidthStored, m_leftCollapsed,
                        kMinLeftWidth, m_rightCollapsed ? 0 : m_rightWidthStored);
}

bool WorkspaceLayout::setRightWidth(int px, int windowInnerWidth)
{
    return setSideWidth(px, windowInnerWidth, m_rightWidthStored, m_rightCollapsed,
                        kMinRightWidth, m_leftCollapsed ? 0 : m_leftWidthStored);
}

bool WorkspaceLayout::setSideWidth(int px, int windowInnerWidth, int &stored, bool &collapsed,
                                   int minWidth, int otherWidth)
{
    bool changed = false;
    if (windowInnerWidth > 0 && m_windowInnerWidth != windowInnerWidth) {
        m_windowInnerWidth = windowInnerWidth;
        changed = true;
    }
    if (px < 120) {
        if (!collapsed) {
            collapsed = true;
            changed = true;
        }
    } else {
        const int maxWidth = std::max(minWidth, m_windowInnerWidth - otherWidth - kMinCenterWidth);
        const int clamped = std::clamp(px, minWidth, maxWidth);
        if (stored != clamped || collapsed) {
            stored = clamped;
            collapsed = false;
            changed = true;
        }
    }
    changed = fillCenterColumns() || changed;
    if (changed) {
        emit geometryChanged();
        commitOrDeferGeometry();
    }
    return true;
}

bool WorkspaceLayout::setSideCollapsed(const QString &zone, bool collapsed)
{
    bool *target = zone == kZoneLeft ? &m_leftCollapsed
                 : zone == kZoneRight ? &m_rightCollapsed
                                      : nullptr;
    if (!target) {
        return false;
    }
    if (*target == collapsed) {
        return true;
    }
    *target = collapsed;
    fillCenterColumns();
    emit geometryChanged();
    commitOrDeferGeometry();
    return true;
}

bool WorkspaceLayout::setRightPage(const QString &page)
{
    if (!isLegalRightPage(page)) {
        return false;
    }
    if (m_rightPage == page) {
        return true;
    }
    m_rightPage = page;
    emit geometryChanged();
    commitOrDeferGeometry();
    return true;
}

bool WorkspaceLayout::setCenterRowRatio(int column, qreal ratio)
{
    if (column < 0 || column >= m_rowRatios.size()) {
        return false;
    }
    const qreal clamped = clampedRowRatio(ratio);
    if (nearlyEqual(m_rowRatios.at(column), clamped)) {
        return true;
    }
    m_rowRatios[column] = clamped;
    emit geometryChanged();
    commitOrDeferGeometry();
    return true;
}

bool WorkspaceLayout::setCenterColumnWidth(int column, int width)
{
    if (column < 0 || column >= m_columnWidths.size()) {
        return false;
    }
    if (m_columnWidths.size() == 1) {
        fillCenterColumns();
        return true;
    }
    if (column + 1 >= m_columnWidths.size()) {
        return false;
    }
    const int pair = m_columnWidths.at(column) + m_columnWidths.at(column + 1);
    const int clamped = clampedCenterColumnWidth(column, width);
    if (clamped == m_columnWidths.at(column)
        && pair - clamped == m_columnWidths.at(column + 1)) {
        return true;
    }
    m_columnWidths[column] = clamped;
    m_columnWidths[column + 1] = pair - clamped;
    emit geometryChanged();
    commitOrDeferGeometry();
    return true;
}

int WorkspaceLayout::clampedCenterColumnWidth(int column, int width) const
{
    if (column < 0 || column + 1 >= m_columnWidths.size()) {
        return 0;
    }
    const int minLeft = tile_grid::columnMinWidth(m_tiles, column);
    const int minRight = tile_grid::columnMinWidth(m_tiles, column + 1);
    const int pair = m_columnWidths.at(column) + m_columnWidths.at(column + 1);
    if (pair < minLeft + minRight) {
        return m_columnWidths.at(column);
    }
    return std::clamp(width, minLeft, pair - minRight);
}

void WorkspaceLayout::beginViewportTransaction()
{
    m_viewportTransactionActive = true;
}

void WorkspaceLayout::commitOrDeferGeometry()
{
    // User seam edits are durable. Snapshotting them into the preview baseline
    // keeps a later window-size preview from rewinding the seam.
    captureCommittedGeometry();
    if (m_viewportTransactionActive) {
        m_viewportCommitPending = true;
        return;
    }
    emit layoutCommitted();
}

LayoutGeometry WorkspaceLayout::currentGeometry() const
{
    return {m_columnWidths, m_rowRatios, m_leftWidthStored, m_leftCollapsed,
            m_rightWidthStored, m_rightCollapsed, m_rightPage};
}

void WorkspaceLayout::captureCommittedGeometry()
{
    m_committed = currentGeometry();
}

bool WorkspaceLayout::previewViewport(int width, int height)
{
    if (width <= 0 || height < kMinRowHeight) {
        return false;
    }
    beginViewportTransaction();
    // Every preview is derived from the last committed geometry. Otherwise
    // rounding and minimum-size clamps accumulate across a resize storm, so
    // the final layout depends on how many intermediate WM_SIZE events were
    // delivered instead of only on the latest viewport.
    if (m_committed.columnWidths.size() == m_columnWidths.size()) {
        m_columnWidths = m_committed.columnWidths;
    }
    if (m_committed.rowRatios.size() == m_rowRatios.size()) {
        m_rowRatios = m_committed.rowRatios;
    }
    bool changed = false;
    if (m_windowInnerWidth != width) {
        m_windowInnerWidth = width;
        changed = true;
    }
    if (m_centerHeight != height) {
        m_centerHeight = height;
        ensureRowRatioCount();
        changed = true;
    }
    const bool durableGeometryChanged = fillCenterColumns();
    changed = durableGeometryChanged || changed;
    m_viewportCommitPending = m_viewportCommitPending || durableGeometryChanged;
    if (changed) {
        emit geometryChanged();
    }
    return true;
}

void WorkspaceLayout::commitViewport()
{
    m_viewportTransactionActive = false;
    if (m_viewportCommitPending) {
        m_viewportCommitPending = false;
        captureCommittedGeometry();
        emit layoutCommitted();
    }
}

QString WorkspaceLayout::toJson() const
{
    // While a viewport transaction is open, the committed geometry is the
    // durable one.
    return layoutToJson({m_tiles, m_viewportTransactionActive ? m_committed : currentGeometry(), m_focusedTileId});
}

bool WorkspaceLayout::restoreFromJson(const QString &json)
{
    LayoutSnapshot stored;
    switch (layoutFromJson(json, &stored)) {
    case LayoutParse::Invalid: return false;
    case LayoutParse::UseDefault: resetToDefault(); return true;
    case LayoutParse::Restored: break;
    }

    beginResetModel();
    m_tiles = stored.tiles;
    m_columnWidths = stored.geometry.columnWidths;
    m_rowRatios = stored.geometry.rowRatios;
    m_leftWidthStored = std::max(kMinLeftWidth, stored.geometry.leftWidthStored);
    m_leftCollapsed = stored.geometry.leftCollapsed;
    m_rightWidthStored = std::max(kMinRightWidth, stored.geometry.rightWidthStored);
    m_rightCollapsed = stored.geometry.rightCollapsed;
    m_rightPage = isLegalRightPage(stored.geometry.rightPage) ? stored.geometry.rightPage : QStringLiteral("files");
    ensureRowRatioCount();
    m_nextId = 1;
    for (const Tile &tile : m_tiles) {
        rememberId(tile.id);
    }
    if (indexOfId(stored.focusedTileId) >= 0)
        m_focusedTileId = stored.focusedTileId;
    else
        m_focusedTileId = m_tiles.isEmpty() ? QString() : m_tiles.constFirst().id;
    m_viewportTransactionActive = false;
    m_viewportCommitPending = false;
    captureCommittedGeometry();
    endResetModel();
    ++m_tilesRevision;
    emit tilesChanged();
    emit geometryChanged();
    emit focusedTileIdChanged();
    return true;
}

void WorkspaceLayout::focusTile(const QString &id)
{
    if (indexOfId(id) < 0) {
        return;
    }
    setFocused(id);
}

bool WorkspaceLayout::setTileSessionId(const QString &id, const QString &sessionId)
{
    const int row = indexOfId(id);
    if (row < 0) {
        return false;
    }
    for (const Tile &tile : m_tiles) {
        if (tile.id != id && !sessionId.isEmpty()
            && tile.sessionId == sessionId) {
            return false;
        }
    }
    if (m_tiles.at(row).sessionId == sessionId) {
        return true;
    }
    m_tiles[row].sessionId = sessionId;
    const QModelIndex idx = index(row, 0);
    emit dataChanged(idx, idx, {SessionIdRole});
    notifyTilesChanged();
    return true;
}

QString WorkspaceLayout::tileIdForSession(const QString &sessionId) const
{
    for (const Tile &tile : m_tiles) {
        if (tile.kind == QLatin1String("terminal")
            && tile.sessionId == sessionId) {
            return tile.id;
        }
    }
    return {};
}

QString WorkspaceLayout::tileIdForKindAndProject(
    const QString &kind, const QString &projectPath) const
{
    for (const Tile &tile : m_tiles) {
        if (tile.kind == kind && tile.projectPath == projectPath) {
            return tile.id;
        }
    }
    return {};
}

int WorkspaceLayout::tileCountForProject(const QString &projectPath) const
{
    return int(std::count_if(m_tiles.cbegin(), m_tiles.cend(), [&projectPath](const Tile &tile) {
        return tile.projectPath == projectPath;
    }));
}

bool WorkspaceLayout::reconcileReferences(const QStringList &scopes, const QHash<QString, QString> &sessionScopes)
{
    bool changed = false;
    for (int i = m_tiles.size() - 1; i >= 0; --i) {
        const Tile &tile = m_tiles.at(i);
        const bool missingProject = tile_grid::isProjectBoundKind(tile.kind) && !scopes.contains(tile.projectPath);
        const auto sessionScope = sessionScopes.constFind(tile.sessionId);
        const bool missingSession = tile.kind == QLatin1String("terminal")
            && (sessionScope == sessionScopes.constEnd() || *sessionScope != tile.projectPath);
        if (!missingProject && !missingSession) {
            continue;
        }
        const Tile removed = tile;
        beginRemoveRows(QModelIndex(), i, i);
        m_tiles.removeAt(i);
        endRemoveRows();
        if (removed.inCenter()) {
            compactCenterColumn(removed.column);
        }
        changed = true;
    }
    if (changed) {
        if (indexOfId(m_focusedTileId) < 0) {
            setFocused(m_tiles.isEmpty() ? QString() : m_tiles.constFirst().id);
        }
        notifyAllTilesChanged();
        emit geometryChanged();
    }
    return changed;
}

QVariantMap WorkspaceLayout::tileToMap(const Tile &tile)
{
    return {
        {QStringLiteral("id"), tile.id},
        {QStringLiteral("kind"), tile.kind},
        {QStringLiteral("zone"), tile.inCenter() ? kZoneCenter : kZoneLeft},
        {QStringLiteral("row"), tile.row},
        {QStringLiteral("rowSpan"), tile.rowSpan},
        {QStringLiteral("column"), tile.column},
        {QStringLiteral("sessionId"), tile.sessionId},
        {QStringLiteral("projectPath"), tile.projectPath},
    };
}

std::optional<WorkspaceLayout::Tile> WorkspaceLayout::tile(const QString &id) const
{
    const int row = indexOfId(id);
    return row < 0 ? std::nullopt : std::optional<Tile>(m_tiles.at(row));
}

bool WorkspaceLayout::centerColumnStacked(int column) const
{
    const QVector<int> occupants = tile_grid::columnOccupants(m_tiles, column);
    const auto stacked =
        std::count_if(occupants.cbegin(), occupants.cend(), [this](int i) { return m_tiles.at(i).rowSpan != 2; });
    return stacked >= 2;
}

QVariantMap WorkspaceLayout::tileById(const QString &id) const
{
    const int row = indexOfId(id);
    return row < 0 ? QVariantMap() : tileToMap(m_tiles.at(row));
}

bool WorkspaceLayout::isLegalRightPage(const QString &page)
{
    return page == QLatin1String("files") || page == QLatin1String("diff")
        || page == QLatin1String("history");
}

int WorkspaceLayout::indexOfId(const QString &id) const
{
    for (int i = 0; i < m_tiles.size(); ++i) {
        if (m_tiles.at(i).id == id) {
            return i;
        }
    }
    return -1;
}

bool WorkspaceLayout::hasProjectsTile() const
{
    return std::any_of(m_tiles.cbegin(), m_tiles.cend(),
                       [](const Tile &tile) { return !tile.inCenter(); });
}

QString WorkspaceLayout::allocateId()
{
    return QStringLiteral("tile-%1").arg(m_nextId++);
}

void WorkspaceLayout::rememberId(const QString &id)
{
    const QString prefix = QStringLiteral("tile-");
    if (!id.startsWith(prefix)) {
        return;
    }
    bool ok = false;
    const int value = QStringView(id).mid(prefix.size()).toInt(&ok);
    if (ok && value >= m_nextId) {
        m_nextId = value + 1;
    }
}

QString WorkspaceLayout::insertNewTile(const Tile &tile)
{
    const int row = m_tiles.size();
    beginInsertRows(QModelIndex(), row, row);
    m_tiles.append(tile);
    endInsertRows();
    if (tile.inCenter()) {
        growColumnCount(std::max(int(m_columnWidths.size()), tile.column + 1));
    } else {
        fillCenterColumns();
    }
    notifyTilesChanged();
    emit geometryChanged();
    setFocused(tile.id);
    return tile.id;
}

void WorkspaceLayout::placeInCenter(Tile &tile)
{
    const int columns = tile_grid::columnCount(m_tiles);
    // Share the first column that holds a single full-height tile.
    for (int column = 0; column < columns; ++column) {
        const QVector<int> occupants = tile_grid::columnOccupants(m_tiles, column);
        const int occupant = occupants.size() == 1 ? occupants.constFirst() : -1;
        if (occupant >= 0 && m_tiles.at(occupant).rowSpan == 2) {
            m_tiles[occupant].row = 0;
            m_tiles[occupant].rowSpan = 1;
            emit dataChanged(index(occupant, 0), index(occupant, 0));
            tile.column = column;
            tile.row = 1;
            tile.rowSpan = 1;
            return;
        }
    }
    // Otherwise open a new full-height column on the right.
    tile.column = columns;
    tile.row = 0;
    tile.rowSpan = 2;
    if (columns == 0) {
        m_columnWidths = {tile_grid::minWidthForKind(tile.kind)};
        m_rowRatios = {0.5};
    }
}

void WorkspaceLayout::compactCenterColumn(int column)
{
    if (tile_grid::vacateColumn(m_tiles, m_columnWidths, m_rowRatios, column)) {
        tile_grid::reconcileWidths(m_columnWidths, m_tiles, centerPixelWidth());
    }
}

void WorkspaceLayout::growColumnCount(int count)
{
    m_columnWidths.resize(count);
    tile_grid::fillNewColumnWidths(m_columnWidths, 1);
    ensureRowRatioCount();
    fillCenterColumns();
}

bool WorkspaceLayout::fillCenterColumns()
{
    if (m_columnWidths.isEmpty()) {
        return false;
    }
    const QVector<int> next = tile_grid::distributeWidths(m_columnWidths, m_tiles, centerPixelWidth());
    if (next == m_columnWidths) {
        return false;
    }
    m_columnWidths = next;
    return true;
}

void WorkspaceLayout::notifyTilesChanged()
{
    ++m_tilesRevision;
    if (m_viewportTransactionActive) {
        m_viewportCommitPending = true;
    } else {
        captureCommittedGeometry();
    }
    emit tilesChanged();
}

void WorkspaceLayout::notifyAllTilesChanged()
{
    if (!m_tiles.isEmpty()) {
        // Empty roles: every role may have changed.
        emit dataChanged(index(0, 0), index(m_tiles.size() - 1, 0));
    }
    notifyTilesChanged();
}

int WorkspaceLayout::centerPixelWidth() const
{
    return std::max(0, m_windowInnerWidth - leftWidth() - rightWidth());
}

qreal WorkspaceLayout::clampedRowRatio(qreal ratio) const
{
    return tile_grid::clampedRowRatio(ratio, m_centerHeight);
}

void WorkspaceLayout::ensureRowRatioCount()
{
    m_rowRatios.resize(m_columnWidths.size(), 0.5);
    for (qreal &ratio : m_rowRatios) {
        ratio = clampedRowRatio(ratio);
    }
}

void WorkspaceLayout::resetToDefault()
{
    beginResetModel();
    ++m_tilesRevision;
    Tile projects;
    projects.id = allocateId();
    projects.kind = kProjectsKind;
    m_tiles = {projects};
    m_columnWidths.clear();
    m_rowRatios.clear();
    m_leftWidthStored = kMinLeftWidth;
    m_leftCollapsed = false;
    m_rightWidthStored = kDefaultRightWidth;
    m_rightCollapsed = true;
    m_rightPage = QStringLiteral("files");
    m_viewportTransactionActive = false;
    m_viewportCommitPending = false;
    captureCommittedGeometry();
    m_focusedTileId = projects.id;
    endResetModel();
    emit tilesChanged();
    emit geometryChanged();
    emit focusedTileIdChanged();
}

void WorkspaceLayout::setFocused(const QString &id)
{
    if (m_focusedTileId == id) {
        return;
    }
    m_focusedTileId = id;
    emit focusedTileIdChanged();
}

} // namespace workspace
} // namespace span
