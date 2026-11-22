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
    // Stored panel widths: the docked width when docked, the overlay width
    // when floating.
    Q_PROPERTY(int leftPanelWidth READ leftPanelWidth NOTIFY geometryChanged FINAL)
    Q_PROPERTY(int rightPanelWidth READ rightPanelWidth NOTIFY geometryChanged FINAL)
    // A side the user keeps docked floats while the window is too narrow for
    // it and the center columns' minimums; derived, never persisted.
    Q_PROPERTY(bool leftFloating READ leftFloating NOTIFY sidesChanged FINAL)
    Q_PROPERTY(bool rightFloating READ rightFloating NOTIFY sidesChanged FINAL)
    Q_PROPERTY(bool leftOverlayOpen READ leftOverlayOpen NOTIFY sidesChanged FINAL)
    Q_PROPERTY(bool rightOverlayOpen READ rightOverlayOpen NOTIFY sidesChanged FINAL)
    // What the side toggles show: docked and open, or floating and slid in.
    Q_PROPERTY(bool leftShown READ leftShown NOTIFY sidesChanged FINAL)
    Q_PROPERTY(bool rightShown READ rightShown NOTIFY sidesChanged FINAL)
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
    int rightWidth() const;
    bool rightCollapsed() const { return m_rightCollapsed; }
    int leftPanelWidth() const { return m_leftWidthStored; }
    int rightPanelWidth() const { return m_rightWidthStored; }
    bool leftFloating() const { return m_leftFloating; }
    bool rightFloating() const { return m_rightFloating; }
    bool leftOverlayOpen() const { return m_leftOverlayOpen; }
    bool rightOverlayOpen() const { return m_rightOverlayOpen; }
    bool leftShown() const;
    bool rightShown() const;
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
    // Docking intent: persisted, and what a wide window shows.
    Q_INVOKABLE bool setSideCollapsed(const QString &zone, bool collapsed);
    // Shows or hides a side the way it currently lives: a docked side
    // changes its docking intent, a floating side slides its overlay in or
    // out and leaves the intent alone. Showing one overlay hides the other.
    Q_INVOKABLE bool setSideShown(const QString &zone, bool shown);
    Q_INVOKABLE bool sideShown(const QString &zone) const;
    Q_INVOKABLE void closeOverlays();
    // The width a side seam drag at `px` previews: the collapse threshold
    // up to what leaves the center columns their minimums.
    Q_INVOKABLE int clampedSideWidth(const QString &zone, int px, int windowInnerWidth) const;
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
    void sidesChanged();

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
    // Re-derives the floating sides from the window width; true when one
    // changed. Called by fillCenterColumns, before it sizes the center.
    bool updateFloating();
    // Which sides float and how wide the docked ones are. A docked side
    // gives way down to its minimum width (the right one first) before it
    // floats; floating is decided at those minimums.
    struct SidePlan {
        bool leftFloats = false;
        bool rightFloats = false;
        int left = 0;
        int right = 0;
    };
    // Floats for `tiles` starting from the current ones (the hysteresis),
    // with the widths that leaves.
    SidePlan planSides(const QVector<Tile> &tiles, int columns) const;
    // The docked widths for the given floats.
    SidePlan sideWidths(const QVector<Tile> &tiles, int columns, bool leftFloats, bool rightFloats) const;
    int centerMinWidth() const;
    int sideMaxWidth(int otherWidth) const;
    int sideState() const;
    void syncSides();
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
    bool m_leftFloating = false;
    bool m_rightFloating = false;
    bool m_leftOverlayOpen = false;
    bool m_rightOverlayOpen = false;
    // Last side state announced through sidesChanged.
    int m_announcedSides = 0;
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
