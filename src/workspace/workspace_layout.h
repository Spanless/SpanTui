#pragma once

#include "workspace/tile_grid.h"
#include "workspace/workspace_constants.h"
#include "workspace/workspace_layout_json.h"

#include <QAbstractListModel>
#include <QHash>
#include <QString>
#include <QVariantList>
#include <QVariantMap>
#include <QVector>

#include <optional>

namespace span {
namespace workspace {

// Tile model of the workspace canvas: owns the tiles, the canvas geometry and
// the viewport transaction. Grid arithmetic lives in tile_grid, the stored
// form in workspace_layout_json.
class WorkspaceLayout final : public QAbstractListModel {
    Q_OBJECT
    Q_PROPERTY(int leftWidth READ leftWidth NOTIFY geometryChanged FINAL)
    Q_PROPERTY(bool leftCollapsed READ leftCollapsed NOTIFY geometryChanged FINAL)
    Q_PROPERTY(int rightWidth READ rightWidth NOTIFY geometryChanged FINAL)
    Q_PROPERTY(bool rightCollapsed READ rightCollapsed NOTIFY geometryChanged FINAL)
    Q_PROPERTY(QString rightPage READ rightPage NOTIFY geometryChanged FINAL)
    Q_PROPERTY(QVariantList centerRowRatios READ centerRowRatios NOTIFY geometryChanged FINAL)
    Q_PROPERTY(QVariantList centerColumnWidths READ centerColumnWidths NOTIFY geometryChanged FINAL)
    Q_PROPERTY(QString focusedTileId READ focusedTileId NOTIFY focusedTileIdChanged FINAL)
    Q_PROPERTY(int tilesRevision READ tilesRevision NOTIFY tilesChanged FINAL)

public:

    enum Role {
        IdRole = Qt::UserRole + 1,
        KindRole,
        ZoneRole,
        RowRole,
        RowSpanRole,
        ColumnRole,
        SessionIdRole,
        ProjectPathRole
    };
    enum DropSlot {
        DropColumn = tile_grid::DropColumn,
        DropUpper = tile_grid::DropUpper,
        DropLower = tile_grid::DropLower,
        DropLeft = tile_grid::DropLeft,
        DropRight = tile_grid::DropRight
    };
    Q_ENUM(DropSlot)

    using Tile = span::workspace::Tile;

    explicit WorkspaceLayout(QObject *parent = nullptr);

    int rowCount(const QModelIndex &parent = QModelIndex()) const override;
    QVariant data(const QModelIndex &index, int role = Qt::DisplayRole) const override;
    QHash<int, QByteArray> roleNames() const override;

    int leftWidth() const;
    bool leftCollapsed() const { return m_leftCollapsed; }
    int rightWidth() const { return m_rightCollapsed ? 0 : m_rightWidthStored; }
    bool rightCollapsed() const { return m_rightCollapsed; }
    QString rightPage() const { return m_rightPage; }
    QVariantList centerRowRatios() const;
    qreal centerRowRatio(int column) const;
    QVariantList centerColumnWidths() const;
    QString focusedTileId() const { return m_focusedTileId; }
    int tilesRevision() const { return m_tilesRevision; }

    // Returns the projects tile, recreating it in the left zone if needed.
    QString showProjectsTile();
    QString addProjectTile(const QString &kind, const QString &projectPath);
    Q_INVOKABLE bool insertCenterTile(const QString &id, int column, int slot);
    Q_INVOKABLE QVariantMap previewCenterInsert(const QString &id, int column, int slot) const;
    Q_INVOKABLE int dropSlotAt(qreal xRatio, qreal yRatio) const;
    bool closeTile(const QString &id);
    Q_INVOKABLE bool setLeftWidth(int px, int windowInnerWidth);
    Q_INVOKABLE bool setRightWidth(int px, int windowInnerWidth);
    Q_INVOKABLE bool setSideCollapsed(const QString &zone, bool collapsed);
    Q_INVOKABLE bool setRightPage(const QString &page);
    Q_INVOKABLE bool setCenterRowRatio(int column, qreal ratio);
    Q_INVOKABLE bool setCenterColumnWidth(int column, int width);
    Q_INVOKABLE int clampedCenterColumnWidth(int column, int width) const;
    Q_INVOKABLE qreal clampedCenterRowRatio(qreal ratio) const { return clampedRowRatio(ratio); }
    // The viewport transaction is driven by one caller (the window transition
    // coordinator, which owns its epoch) in order: begin, previews, commit.
    void beginViewportTransaction();
    bool previewViewport(int width, int height);
    void commitViewport();
    QString toJson() const;
    bool restoreFromJson(const QString &json);
    void focusTile(const QString &id);
    bool setTileSessionId(const QString &id, const QString &sessionId);
    QString tileIdForSession(const QString &sessionId) const;
    QString tileIdForKindAndProject(const QString &kind, const QString &projectPath) const;
    int tileCountForProject(const QString &projectPath) const;
    // Drops tiles whose scope is not listed, and terminal tiles whose
    // session is gone or belongs to another scope (`sessionScopes`: session
    // id -> scope key).
    bool reconcileReferences(const QStringList &scopes, const QHash<QString, QString> &sessionScopes);

    const QVector<Tile> &tiles() const { return m_tiles; }
    // nullopt when no tile has this id.
    std::optional<Tile> tile(const QString &id) const;
    Q_INVOKABLE bool centerColumnStacked(int column) const;
    // The tile as a map for QML; empty when no tile has this id.
    Q_INVOKABLE QVariantMap tileById(const QString &id) const;
signals:
    void tilesChanged();
    void geometryChanged();
    void layoutCommitted();
    void focusedTileIdChanged();

private:
    static bool isLegalRightPage(const QString &page);
    static QVariantMap tileToMap(const Tile &tile);

    int indexOfId(const QString &id) const;
    bool hasProjectsTile() const;
    QString allocateId();
    void rememberId(const QString &id);
    QString insertNewTile(const Tile &tile);
    void placeInCenter(Tile &tile);
    bool setSideWidth(int px, int windowInnerWidth, int &stored, bool &collapsed,
                      int minWidth, int otherWidth);
    void compactCenterColumn(int column);
    void growColumnCount(int count);
    // Distributes the free center width over the columns; true when a width
    // changed.
    bool fillCenterColumns();
    void commitOrDeferGeometry();
    LayoutGeometry currentGeometry() const;
    void captureCommittedGeometry();
    void notifyTilesChanged();
    void notifyAllTilesChanged();
    int centerPixelWidth() const;
    qreal clampedRowRatio(qreal ratio) const;
    void ensureRowRatioCount();
    void resetToDefault();
    void setFocused(const QString &id);

    QVector<Tile> m_tiles;
    QVector<int> m_columnWidths;
    QVector<qreal> m_rowRatios;
    int m_leftWidthStored = kMinLeftWidth;
    bool m_leftCollapsed = false;
    int m_rightWidthStored = kDefaultRightWidth;
    bool m_rightCollapsed = true;
    QString m_rightPage = QStringLiteral("files");
    int m_windowInnerWidth = kDefaultWindowWidth;
    int m_centerHeight = kDefaultCenterHeight;
    bool m_viewportCommitPending = false;
    bool m_viewportTransactionActive = false;
    // Last durable geometry: previews derive from it and toJson() persists it
    // while a viewport transaction is open.
    LayoutGeometry m_committed;
    int m_nextId = 1;
    QString m_focusedTileId;
    int m_tilesRevision = 0;
};

} // namespace workspace
} // namespace span
