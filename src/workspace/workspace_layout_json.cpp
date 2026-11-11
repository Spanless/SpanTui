#include "workspace/workspace_layout_json.h"

#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSet>

#include <cmath>

namespace span::workspace {
namespace {

// v4 files may still carry the retired "zone"/"colSpan" fields; both are
// derived now and ignored on restore.
constexpr int kLayoutVersion = 4;

} // namespace

QString layoutToJson(const LayoutSnapshot &snapshot)
{
    QJsonArray tiles;
    for (const Tile &tile : snapshot.tiles) {
        QJsonObject object{
            {QStringLiteral("id"), tile.id},         {QStringLiteral("kind"), tile.kind},
            {QStringLiteral("row"), tile.row},       {QStringLiteral("rowSpan"), tile.rowSpan},
            {QStringLiteral("column"), tile.column},
        };
        if (!tile.sessionId.isEmpty()) {
            object.insert(QStringLiteral("sessionId"), tile.sessionId);
        }
        if (!tile.projectPath.isEmpty()) {
            object.insert(QStringLiteral("projectPath"), tile.projectPath);
        }
        tiles.append(object);
    }
    const LayoutGeometry &geometry = snapshot.geometry;
    const int columns = tile_grid::columnCount(snapshot.tiles);
    QJsonArray rowRatios;
    QJsonArray widths;
    for (int column = 0; column < columns; ++column) {
        const qreal ratio = geometry.rowRatios.value(column, 0.5);
        rowRatios.append(std::isfinite(ratio) && ratio > 0 && ratio < 1 ? ratio : 0.5);
        const int width = geometry.columnWidths.value(column, 0);
        widths.append(width > 0 ? width : kMinCenterWidth);
    }
    const QJsonObject root{
        {QStringLiteral("v"), kLayoutVersion},
        {QStringLiteral("tiles"), tiles},
        {QStringLiteral("leftWidth"), geometry.leftWidthStored},
        {QStringLiteral("leftCollapsed"), geometry.leftCollapsed},
        {QStringLiteral("rightWidth"), geometry.rightWidthStored},
        {QStringLiteral("rightCollapsed"), geometry.rightCollapsed},
        {QStringLiteral("rightPage"), geometry.rightPage},
        {QStringLiteral("focusedTileId"), snapshot.focusedTileId},
        {QStringLiteral("rowRatios"), rowRatios},
        {QStringLiteral("columnWidths"), widths},
    };
    return QString::fromUtf8(QJsonDocument(root).toJson(QJsonDocument::Compact));
}

LayoutParse layoutFromJson(const QString &json, LayoutSnapshot *snapshot)
{
    if (json.trimmed().isEmpty()) {
        return LayoutParse::UseDefault;
    }
    QJsonParseError error;
    const QJsonDocument document = QJsonDocument::fromJson(json.toUtf8(), &error);
    if (error.error != QJsonParseError::NoError || !document.isObject()) {
        return LayoutParse::Invalid;
    }
    const QJsonObject root = document.object();
    if (root.value(QStringLiteral("v")).toInt() != kLayoutVersion) {
        return LayoutParse::UseDefault;
    }
    const QJsonValue tileValues = root.value(QStringLiteral("tiles"));
    const QJsonValue widthValues = root.value(QStringLiteral("columnWidths"));
    const QJsonValue ratioValues = root.value(QStringLiteral("rowRatios"));
    if (!tileValues.isArray() || !widthValues.isArray() || !ratioValues.isArray()) {
        return LayoutParse::Invalid;
    }

    QVector<Tile> parsed;
    QSet<QString> ids;
    QSet<QString> terminalSessions;
    QSet<QString> occupied;
    bool hasProjects = false;
    for (const QJsonValue &value : tileValues.toArray()) {
        if (!value.isObject()) {
            return LayoutParse::Invalid;
        }
        const QJsonObject object = value.toObject();
        Tile tile;
        tile.id = object.value(QStringLiteral("id")).toString();
        tile.kind = object.value(QStringLiteral("kind")).toString();
        tile.row = object.value(QStringLiteral("row")).toInt(-1);
        tile.rowSpan = object.value(QStringLiteral("rowSpan")).toInt(-1);
        tile.column = object.value(QStringLiteral("column")).toInt(-1);
        tile.sessionId = object.value(QStringLiteral("sessionId")).toString();
        tile.projectPath = object.value(QStringLiteral("projectPath")).toString();
        const bool projectBound = tile_grid::isProjectBoundKind(tile.kind);
        const bool legalKind = !tile.inCenter() || projectBound;
        if (tile.id.isEmpty() || ids.contains(tile.id) || !legalKind || tile.row < 0 || tile.row > 1 || tile.rowSpan < 1
            || tile.rowSpan > 2 || tile.column < 0 || (projectBound && tile.projectPath.isEmpty())
            || (tile.kind == QLatin1String("terminal") && tile.sessionId.isEmpty())) {
            return LayoutParse::Invalid;
        }
        if (!tile.inCenter()) {
            if (hasProjects) {
                return LayoutParse::Invalid;
            }
            hasProjects = true;
            tile.row = 0;
            tile.rowSpan = 2;
            tile.column = 0;
        } else {
            const QString base = QString::number(tile.column) + QLatin1Char(':');
            const QStringList cells = tile.rowSpan == 2
                ? QStringList{base + QStringLiteral("0"), base + QStringLiteral("1")}
                : QStringList{base + QString::number(tile.row)};
            for (const QString &cell : cells) {
                if (occupied.contains(cell)) {
                    return LayoutParse::Invalid;
                }
                occupied.insert(cell);
            }
        }
        if (!tile.sessionId.isEmpty()) {
            if (terminalSessions.contains(tile.sessionId)) {
                return LayoutParse::Invalid;
            }
            terminalSessions.insert(tile.sessionId);
        }
        ids.insert(tile.id);
        parsed.append(tile);
    }

    const QJsonArray widthArray = widthValues.toArray();
    const QJsonArray ratioArray = ratioValues.toArray();
    const int columnCount = tile_grid::columnCount(parsed);
    if (widthArray.size() != columnCount || ratioArray.size() != columnCount) {
        return LayoutParse::Invalid;
    }
    // Drop empty columns so every stored column holds at least one tile.
    QVector<int> widths;
    QVector<qreal> ratios;
    QHash<int, int> columnRemap;
    for (int column = 0; column < columnCount; ++column) {
        if (tile_grid::columnOccupants(parsed, column).isEmpty()) {
            continue;
        }
        const int width = widthArray.at(column).toInt();
        if (width < tile_grid::columnMinWidth(parsed, column)) {
            return LayoutParse::Invalid;
        }
        columnRemap.insert(column, widths.size());
        widths.append(width);
        ratios.append(ratioArray.at(column).toDouble(0.5));
    }
    for (Tile &tile : parsed) {
        if (tile.inCenter()) {
            tile.column = columnRemap.value(tile.column);
        }
    }

    snapshot->tiles = parsed;
    snapshot->geometry.columnWidths = widths;
    snapshot->geometry.rowRatios = ratios;
    snapshot->geometry.leftWidthStored = root.value(QStringLiteral("leftWidth")).toInt(kMinLeftWidth);
    snapshot->geometry.leftCollapsed = root.value(QStringLiteral("leftCollapsed")).toBool();
    snapshot->geometry.rightWidthStored = root.value(QStringLiteral("rightWidth")).toInt(kDefaultRightWidth);
    snapshot->geometry.rightCollapsed = root.value(QStringLiteral("rightCollapsed")).toBool(true);
    snapshot->geometry.rightPage = root.value(QStringLiteral("rightPage")).toString();
    snapshot->focusedTileId = root.value(QStringLiteral("focusedTileId")).toString();
    return LayoutParse::Restored;
}

} // namespace span::workspace
