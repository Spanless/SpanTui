#include "workspace/workspace_constants.h"
#include "workspace/workspace_layout.h"

#include <QAbstractItemModel>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSignalSpy>
#include <QVariantList>
#include <QVariantMap>

#include <QTest>

using namespace span::workspace;

class WorkspaceLayoutTest final : public QObject {
    Q_OBJECT

private slots:
    void defaultHasProjectManagerOnLeft();
    void secondTileSplitsIntoTwoRows();
    void thirdTileAddsFullHeightRightColumn();
    void fourthTileCompletesTwoByTwo();
    void fifthTileAppendsFullHeightColumn();
    void projectsKindReusesExistingId();
    void unknownKindsAreRejected();
    void restoreRejectsUnknownKinds();
    void rightPanelToggleAndWidth();
    void clampsColResizeToMinWidth();
    void centerColumnsFillRemainingWindowWidth();
    void wideningWindowStretchesCenterColumns();
    void columnSeamStealsFromNeighborWithoutGap();
    void narrowWindowSharesCenterWidthByMinimumRatio();
    void narrowingFloatsRightSideThenLeft();
    void wideningDocksSidesBackPastHysteresis();
    void floatingSideTogglesOverlayNotIntent();
    void showingSideInNarrowWindowSlidesItIn();
    void squeezedLayoutRestoresFromJson();
    void clampedColumnWidthDoesNotMutate();
    void stackedColumnReportsTwoRows();
    void clampsRowRatioToMinHeight();
    void newColumnSplitsEquallyWhenNeverDragged();
    void addedColumnKeepsDraggedWidthRatios();
    void insertColumnShiftsExisting();
    void insertUpperSplitsFullColumn();
    void insertLowerPutsTileUnderFullColumn();
    void bottomSmallMovesToUpperInSameColumn();
    void insertUpperIntoTwoTileColumnDisplaces();
    void previewMatchesCommittedInsert();
    void insertCenterRejectsMissingAndProjects();
    void dropOnOwnFullColumnIsNoOp();
    void dropOnOwnStackedSlotIsNoOp();
    void dropLowerOnOwnStackedColumnSwaps();
    void dropSlotAtUsesEdgeColumnsAndVerticalHalves();
    void roundTripsJsonWithTwoByTwo();
    void rowSplitIsIndependentPerColumn();
    void restoreRejectsMalformedJson();
    void viewportPreviewIsTransientUntilCommit();
    void paneSeamSurvivesSameSizePreviewDuringTransaction();
    void dropRightAtRightmostEdgeDoesNotCreateGhostColumn();
    void dropBesideColumnSplitsItEvenly();
    void dropBesideColumnKeepsOtherColumnWidths();
    void clampedSideWidthDoesNotExceedWindowLimit();
};

namespace {

QModelIndex tileIndex(const WorkspaceLayout &layout, const QString &id)
{
    for (int row = 0; row < layout.rowCount(); ++row) {
        const QModelIndex index = layout.index(row, 0);
        if (layout.data(index, WorkspaceLayout::IdRole).toString() == id) {
            return index;
        }
    }
    return {};
}

QString roleString(const WorkspaceLayout &layout, const QString &id, int role)
{
    return layout.data(tileIndex(layout, id), role).toString();
}

int roleInt(const WorkspaceLayout &layout, const QString &id, int role)
{
    return layout.data(tileIndex(layout, id), role).toInt();
}

QString addCenterTerminal(WorkspaceLayout &layout)
{
    const QString id = layout.addProjectTile(QStringLiteral("terminal"),
                                             QStringLiteral("C:/work"));
    if (id.isEmpty()) {
        return {};
    }
    static int sessionSerial = 0;
    layout.setTileSessionId(id, QStringLiteral("session-%1").arg(++sessionSerial));
    return id;
}

// True when the center tiles use exactly the columns 0..columnCount-1.
bool centerColumnsContiguous(const WorkspaceLayout &layout, int columnCount)
{
    QSet<int> columns;
    for (int row = 0; row < layout.rowCount(); ++row) {
        const QModelIndex idx = layout.index(row, 0);
        if (idx.data(WorkspaceLayout::ZoneRole).toString() == QStringLiteral("center"))
            columns.insert(idx.data(WorkspaceLayout::ColumnRole).toInt());
    }
    for (int column = 0; column < columnCount; ++column) {
        if (!columns.contains(column)) return false;
    }
    return columns.size() == columnCount;
}

int occupiedCenterWidth(const WorkspaceLayout &layout)
{
    const QVariantList widths = layout.centerColumnWidths();
    int sum = 0;
    for (const QVariant &width : widths) {
        sum += width.toInt();
    }
    return sum;
}

// "id kind zone row+rowSpan column" for each tile, sorted by id.
QStringList tilePlacements(const WorkspaceLayout &layout)
{
    QStringList placements;
    for (int row = 0; row < layout.rowCount(); ++row) {
        const QModelIndex idx = layout.index(row, 0);
        placements.append(QStringLiteral("%1 %2 %3 %4+%5 %6")
                              .arg(idx.data(WorkspaceLayout::IdRole).toString(),
                                   idx.data(WorkspaceLayout::KindRole).toString(),
                                   idx.data(WorkspaceLayout::ZoneRole).toString())
                              .arg(idx.data(WorkspaceLayout::RowRole).toInt())
                              .arg(idx.data(WorkspaceLayout::RowSpanRole).toInt())
                              .arg(idx.data(WorkspaceLayout::ColumnRole).toInt()));
    }
    placements.sort();
    return placements;
}

// The drop slots in `dropSlots` whose preview or insert of `id` at `column`
// is accepted.
QList<int> acceptedDropSlots(WorkspaceLayout &layout, const QString &id, int column, const QList<int> &dropSlots)
{
    QList<int> accepted;
    for (int slot : dropSlots) {
        if (layout.previewCenterInsert(id, column, slot).value(QStringLiteral("ok")).toBool()
            || layout.insertCenterTile(id, column, slot))
            accepted.append(slot);
    }
    return accepted;
}

void fillTwoByTwo(WorkspaceLayout &layout, QStringList *ids)
{
    ids->append(addCenterTerminal(layout));
    ids->append(addCenterTerminal(layout));
    ids->append(addCenterTerminal(layout));
    ids->append(addCenterTerminal(layout));
}

} // namespace

void WorkspaceLayoutTest::defaultHasProjectManagerOnLeft()
{
    WorkspaceLayout layout;
    QCOMPARE(layout.rowCount(), 1);
    const QString id = layout.data(layout.index(0, 0), WorkspaceLayout::IdRole).toString();
    QVERIFY(!id.isEmpty());
    QCOMPARE(roleString(layout, id, WorkspaceLayout::KindRole), QStringLiteral("projects"));
    QCOMPARE(roleString(layout, id, WorkspaceLayout::ZoneRole), QStringLiteral("left"));
    QCOMPARE(roleInt(layout, id, WorkspaceLayout::RowRole), 0);
    QCOMPARE(roleInt(layout, id, WorkspaceLayout::RowSpanRole), 2);
    QCOMPARE(roleInt(layout, id, WorkspaceLayout::ColumnRole), 0);
    QVERIFY(layout.leftWidth() >= kMinLeftWidth);
    QVERIFY(layout.centerColumnWidths().isEmpty());
}

void WorkspaceLayoutTest::secondTileSplitsIntoTwoRows()
{
    WorkspaceLayout layout;
    const QString first = addCenterTerminal(layout);
    const QString second = addCenterTerminal(layout);
    QVERIFY(!first.isEmpty());
    QVERIFY(!second.isEmpty());
    QVERIFY(second != first);
    QCOMPARE(layout.rowCount(), 3);
    QCOMPARE(roleInt(layout, first, WorkspaceLayout::RowRole), 0);
    QCOMPARE(roleInt(layout, first, WorkspaceLayout::RowSpanRole), 1);
    QCOMPARE(roleInt(layout, first, WorkspaceLayout::ColumnRole), 0);
    QCOMPARE(roleInt(layout, second, WorkspaceLayout::RowRole), 1);
    QCOMPARE(roleInt(layout, second, WorkspaceLayout::RowSpanRole), 1);
    QCOMPARE(roleInt(layout, second, WorkspaceLayout::ColumnRole), 0);
    QCOMPARE(roleString(layout, second, WorkspaceLayout::ZoneRole), QStringLiteral("center"));
}

void WorkspaceLayoutTest::thirdTileAddsFullHeightRightColumn()
{
    WorkspaceLayout layout;
    // Wide enough for two terminal columns at their minimum.
    QVERIFY(layout.setLeftWidth(250, 1600));
    addCenterTerminal(layout);
    addCenterTerminal(layout);
    const QString third = addCenterTerminal(layout);
    QCOMPARE(layout.rowCount(), 4);
    QCOMPARE(roleInt(layout, third, WorkspaceLayout::ColumnRole), 1);
    QCOMPARE(roleInt(layout, third, WorkspaceLayout::RowRole), 0);
    QCOMPARE(roleInt(layout, third, WorkspaceLayout::RowSpanRole), 2);
    QCOMPARE(layout.centerColumnWidths().size(), 2);
    QVERIFY(layout.centerColumnWidths().at(0).toInt() >= kMinCenterWidth);
    QVERIFY(layout.centerColumnWidths().at(1).toInt() >= kMinCenterWidth);
}

void WorkspaceLayoutTest::fourthTileCompletesTwoByTwo()
{
    WorkspaceLayout layout;
    addCenterTerminal(layout);
    addCenterTerminal(layout);
    const QString third = addCenterTerminal(layout);
    const QString fourth = addCenterTerminal(layout);
    QCOMPARE(layout.rowCount(), 5);
    QCOMPARE(roleInt(layout, third, WorkspaceLayout::RowSpanRole), 1);
    QCOMPARE(roleInt(layout, third, WorkspaceLayout::RowRole), 0);
    QCOMPARE(roleInt(layout, third, WorkspaceLayout::ColumnRole), 1);
    QCOMPARE(roleInt(layout, fourth, WorkspaceLayout::RowRole), 1);
    QCOMPARE(roleInt(layout, fourth, WorkspaceLayout::RowSpanRole), 1);
    QCOMPARE(roleInt(layout, fourth, WorkspaceLayout::ColumnRole), 1);
}

void WorkspaceLayoutTest::fifthTileAppendsFullHeightColumn()
{
    WorkspaceLayout layout;
    addCenterTerminal(layout);
    addCenterTerminal(layout);
    addCenterTerminal(layout);
    addCenterTerminal(layout);
    const QString fifth = addCenterTerminal(layout);
    QCOMPARE(layout.rowCount(), 6);
    QCOMPARE(roleInt(layout, fifth, WorkspaceLayout::ColumnRole), 2);
    QCOMPARE(roleInt(layout, fifth, WorkspaceLayout::RowSpanRole), 2);
    QCOMPARE(layout.centerColumnWidths().size(), 3);
}

void WorkspaceLayoutTest::projectsKindReusesExistingId()
{
    WorkspaceLayout layout;
    QCOMPARE(layout.addProjectTile(QStringLiteral("projects"), QStringLiteral("C:/work")),
             QString());
    QCOMPARE(layout.rowCount(), 1);
    const QString first = layout.showProjectsTile();
    const QString again = layout.showProjectsTile();
    QCOMPARE(first, again);
    QCOMPARE(layout.rowCount(), 1);
    QCOMPARE(roleString(layout, first, WorkspaceLayout::KindRole), QStringLiteral("projects"));
}

void WorkspaceLayoutTest::unknownKindsAreRejected()
{
    WorkspaceLayout layout;
    QCOMPARE(layout.addProjectTile(QStringLiteral("files"), QStringLiteral("C:/work")),
             QString());
    QCOMPARE(layout.addProjectTile(QStringLiteral("not-a-kind"), QStringLiteral("C:/work")),
             QString());
    QCOMPARE(layout.rowCount(), 1);
}

void WorkspaceLayoutTest::restoreRejectsUnknownKinds()
{
    WorkspaceLayout layout;
    const QString terminal = addCenterTerminal(layout);
    QJsonObject root = QJsonDocument::fromJson(layout.toJson().toUtf8()).object();
    QJsonArray tiles = root.value(QStringLiteral("tiles")).toArray();
    tiles.append(QJsonObject{
        {QStringLiteral("id"), QStringLiteral("tile-retired")},
        {QStringLiteral("kind"), QStringLiteral("gitCommit")},
        {QStringLiteral("row"), 0},
        {QStringLiteral("rowSpan"), 2},
        {QStringLiteral("column"), 1},
        {QStringLiteral("projectPath"), QStringLiteral("C:/work")},
    });
    root.insert(QStringLiteral("tiles"), tiles);
    QJsonArray widths = root.value(QStringLiteral("columnWidths")).toArray();
    widths.append(440);
    root.insert(QStringLiteral("columnWidths"), widths);
    QJsonArray ratios = root.value(QStringLiteral("rowRatios")).toArray();
    ratios.append(0.5);
    root.insert(QStringLiteral("rowRatios"), ratios);

    WorkspaceLayout restored;
    QVERIFY(!restored.restoreFromJson(
        QString::fromUtf8(QJsonDocument(root).toJson(QJsonDocument::Compact))));
    QCOMPARE(restored.rowCount(), 1);
    QVERIFY(!terminal.isEmpty());
}

void WorkspaceLayoutTest::rightPanelToggleAndWidth()
{
    WorkspaceLayout layout;
    // Wide enough to dock both sides.
    QVERIFY(layout.setLeftWidth(kMinLeftWidth, 1600));
    QVERIFY(layout.rightCollapsed());
    QCOMPARE(layout.rightWidth(), 0);
    QCOMPARE(layout.rightPage(), QStringLiteral("files"));
    QVERIFY(layout.setSideCollapsed(QStringLiteral("right"), false));
    QVERIFY(!layout.rightCollapsed());
    // A never-resized dock opens at the default width, not the minimum.
    QCOMPARE(layout.rightWidth(), kDefaultRightWidth);
    QVERIFY(layout.setRightPage(QStringLiteral("diff")));
    QCOMPARE(layout.rightPage(), QStringLiteral("diff"));
    QVERIFY(layout.setSideCollapsed(QStringLiteral("right"), true));
    QVERIFY(layout.rightCollapsed());
    QVERIFY(!layout.setRightPage(QStringLiteral("git")));
    QVERIFY(layout.setRightPage(QStringLiteral("history")));
    QCOMPARE(layout.rightPage(), QStringLiteral("history"));
    QVERIFY(!layout.setRightPage(QStringLiteral("bogus")));
    QCOMPARE(layout.rightWidth(), 0);
    QVERIFY(layout.setRightWidth(280, 1600));
    QVERIFY(!layout.rightCollapsed());
    QCOMPARE(layout.rightWidth(), 280);
    QVERIFY(layout.setSideCollapsed(QStringLiteral("right"), true));
    QCOMPARE(layout.rightWidth(), 0);
}

void WorkspaceLayoutTest::clampsColResizeToMinWidth()
{
    WorkspaceLayout layout;
    // Wide enough for two terminal columns at their minimum.
    QVERIFY(layout.setLeftWidth(250, 1600));
    addCenterTerminal(layout);
    addCenterTerminal(layout);
    addCenterTerminal(layout);
    QCOMPARE(layout.centerColumnWidths().size(), 2);
    const int pair = occupiedCenterWidth(layout);
    QVERIFY(layout.setCenterColumnWidth(0, 100));
    QCOMPARE(layout.centerColumnWidths().at(0).toInt(), kMinCenterWidth);
    QCOMPARE(layout.centerColumnWidths().at(1).toInt(), pair - kMinCenterWidth);
}

void WorkspaceLayoutTest::centerColumnsFillRemainingWindowWidth()
{
    WorkspaceLayout layout;
    addCenterTerminal(layout);
    addCenterTerminal(layout);
    addCenterTerminal(layout);
    QCOMPARE(layout.centerColumnWidths().size(), 2);
    QVERIFY(layout.setLeftWidth(250, 1600));
    QCOMPARE(layout.leftWidth(), 250);
    QCOMPARE(occupiedCenterWidth(layout), 1350);
    QVERIFY(layout.setRightWidth(240, 1600));
    QCOMPARE(layout.rightWidth(), 240);
    QCOMPARE(occupiedCenterWidth(layout), 1110);
    QVERIFY(layout.centerColumnWidths().at(0).toInt() >= kMinCenterWidth);
    QVERIFY(layout.centerColumnWidths().at(1).toInt() >= kMinCenterWidth);
}

void WorkspaceLayoutTest::wideningWindowStretchesCenterColumns()
{
    WorkspaceLayout layout;
    addCenterTerminal(layout);
    addCenterTerminal(layout);
    addCenterTerminal(layout);
    QVERIFY(layout.setLeftWidth(250, 1200));
    QCOMPARE(occupiedCenterWidth(layout), 950);
    const int firstLeft = layout.centerColumnWidths().at(0).toInt();
    const int firstRight = layout.centerColumnWidths().at(1).toInt();
    QVERIFY(layout.setLeftWidth(250, 1600));
    QCOMPARE(occupiedCenterWidth(layout), 1350);
    QVERIFY(layout.centerColumnWidths().at(0).toInt() > firstLeft);
    QVERIFY(layout.centerColumnWidths().at(1).toInt() > firstRight);
}

void WorkspaceLayoutTest::columnSeamStealsFromNeighborWithoutGap()
{
    WorkspaceLayout layout;
    addCenterTerminal(layout);
    addCenterTerminal(layout);
    addCenterTerminal(layout);
    QVERIFY(layout.setLeftWidth(250, 1600));
    const int beforeLeft = layout.centerColumnWidths().at(0).toInt();
    const int beforeRight = layout.centerColumnWidths().at(1).toInt();
    const int occupied = occupiedCenterWidth(layout);
    QVERIFY(layout.setCenterColumnWidth(0, beforeLeft + 80));
    QCOMPARE(occupiedCenterWidth(layout), occupied);
    QCOMPARE(layout.centerColumnWidths().at(0).toInt(), beforeLeft + 80);
    QCOMPARE(layout.centerColumnWidths().at(1).toInt(), beforeRight - 80);
}

void WorkspaceLayoutTest::narrowWindowSharesCenterWidthByMinimumRatio()
{
    WorkspaceLayout layout;
    QStringList ids;
    fillTwoByTwo(layout, &ids);
    layout.addProjectTile(QStringLiteral("editor"), QStringLiteral("C:/work"));
    QCOMPARE(layout.centerColumnWidths().size(), 3);
    // Minimums 440 + 440 (terminal columns) + 320 (editor) = 1200. The
    // sidebar floats, and the 1010 window is still shared in that ratio.
    QVERIFY(layout.previewViewport(1010, 640));
    layout.commitViewport();
    QVERIFY(layout.leftFloating());
    QCOMPARE(occupiedCenterWidth(layout), 1010);
    QCOMPARE(layout.centerColumnWidths().at(0).toInt(), 370);
    QCOMPARE(layout.centerColumnWidths().at(1).toInt(), 370);
    QCOMPARE(layout.centerColumnWidths().at(2).toInt(), 270);
}

void WorkspaceLayoutTest::narrowingFloatsRightSideThenLeft()
{
    WorkspaceLayout layout;
    addCenterTerminal(layout);
    QVERIFY(layout.setLeftWidth(300, 1600));
    QVERIFY(layout.setRightWidth(300, 1600));
    QVERIFY(!layout.leftFloating());
    QVERIFY(!layout.rightFloating());

    // 300 + 300 + 440 = 1040: below it the docked sides give way, the right
    // one first, down to their minimums.
    QVERIFY(layout.previewViewport(1000, 640));
    layout.commitViewport();
    QVERIFY(!layout.rightFloating());
    QCOMPARE(layout.rightWidth(), 260);
    QCOMPARE(layout.leftWidth(), 300);
    QVERIFY(layout.previewViewport(960, 640));
    layout.commitViewport();
    QCOMPARE(layout.rightWidth(), kMinRightWidth);
    QCOMPARE(layout.leftWidth(), 280);
    QCOMPARE(occupiedCenterWidth(layout), kMinCenterWidth);
    QCOMPARE(layout.rightPanelWidth(), 300);

    // 250 + 240 + 440 = 930: the right side floats, and the left takes its
    // own width again.
    QVERIFY(layout.previewViewport(920, 640));
    layout.commitViewport();
    QVERIFY(layout.rightFloating());
    QVERIFY(!layout.leftFloating());
    QCOMPARE(layout.rightWidth(), 0);
    QCOMPARE(layout.leftWidth(), 300);
    QCOMPARE(occupiedCenterWidth(layout), 620);

    // 250 + 440 = 690: then the left one.
    QVERIFY(layout.previewViewport(680, 640));
    layout.commitViewport();
    QVERIFY(layout.leftFloating());
    QCOMPARE(layout.leftWidth(), 0);
    QCOMPARE(occupiedCenterWidth(layout), 680);
    QVERIFY(!layout.leftCollapsed());
    QVERIFY(!layout.rightCollapsed());
}

void WorkspaceLayoutTest::wideningDocksSidesBackPastHysteresis()
{
    WorkspaceLayout layout;
    addCenterTerminal(layout);
    QVERIFY(layout.setLeftWidth(250, 1600));
    QVERIFY(layout.setRightWidth(300, 1600));
    QVERIFY(layout.previewViewport(680, 640));
    QVERIFY(layout.leftFloating());
    QVERIFY(layout.rightFloating());

    // Room for the left side, but not the hysteresis on top: still floating.
    QVERIFY(layout.previewViewport(700, 640));
    QVERIFY(layout.leftFloating());
    QVERIFY(layout.previewViewport(kMinLeftWidth + kMinCenterWidth + kSideDockHysteresis, 640));
    QVERIFY(!layout.leftFloating());
    QVERIFY(layout.rightFloating());
    // The right side docks once it fits at its minimum, narrower than stored.
    const int docksRight = kMinLeftWidth + kMinRightWidth + kMinCenterWidth + kSideDockHysteresis;
    QVERIFY(layout.previewViewport(docksRight - 1, 640));
    QVERIFY(layout.rightFloating());
    QVERIFY(layout.previewViewport(docksRight, 640));
    QVERIFY(!layout.rightFloating());
    QCOMPARE(layout.rightWidth(), docksRight - 250 - kMinCenterWidth);
    QVERIFY(layout.previewViewport(1600, 640));
    layout.commitViewport();
    QCOMPARE(layout.leftWidth(), 250);
    QCOMPARE(layout.rightWidth(), 300);
}

void WorkspaceLayoutTest::floatingSideTogglesOverlayNotIntent()
{
    WorkspaceLayout layout;
    addCenterTerminal(layout);
    QVERIFY(layout.setRightWidth(300, 1600));
    QVERIFY(layout.previewViewport(900, 640));
    layout.commitViewport();
    QVERIFY(layout.rightFloating());
    QVERIFY(!layout.rightShown());
    const QString durable = layout.toJson();
    QSignalSpy sides(&layout, &WorkspaceLayout::sidesChanged);

    QVERIFY(layout.setSideShown(QStringLiteral("right"), true));
    QVERIFY(layout.rightOverlayOpen());
    QVERIFY(layout.rightShown());
    QCOMPARE(sides.count(), 1);
    QCOMPARE(layout.rightWidth(), 0);
    QCOMPARE(layout.toJson(), durable);

    QVERIFY(layout.setSideShown(QStringLiteral("right"), false));
    QVERIFY(!layout.rightShown());
    QVERIFY(!layout.rightCollapsed());
    QCOMPARE(layout.toJson(), durable);

    // Widening docks it and drops the overlay.
    QVERIFY(layout.setSideShown(QStringLiteral("right"), true));
    QVERIFY(layout.previewViewport(1600, 640));
    layout.commitViewport();
    QVERIFY(!layout.rightFloating());
    QVERIFY(!layout.rightOverlayOpen());
    QVERIFY(layout.rightShown());
}

void WorkspaceLayoutTest::showingSideInNarrowWindowSlidesItIn()
{
    WorkspaceLayout layout;
    addCenterTerminal(layout);
    QVERIFY(layout.previewViewport(900, 640));
    layout.commitViewport();
    QVERIFY(layout.rightCollapsed());
    // 900 holds the sidebar and one terminal, not even a 240 dock on top.
    QVERIFY(layout.setSideShown(QStringLiteral("right"), true));
    QVERIFY(!layout.rightCollapsed());
    QVERIFY(layout.rightFloating());
    QVERIFY(layout.rightOverlayOpen());
    QCOMPARE(layout.rightPanelWidth(), kDefaultRightWidth);
    QCOMPARE(occupiedCenterWidth(layout), 650);

    layout.closeOverlays();
    QVERIFY(!layout.rightShown());
    QVERIFY(layout.sideShown(QStringLiteral("left")));
}

void WorkspaceLayoutTest::squeezedLayoutRestoresFromJson()
{
    WorkspaceLayout layout;
    addCenterTerminal(layout);
    addCenterTerminal(layout);
    addCenterTerminal(layout);
    QVERIFY(layout.setLeftWidth(250, 800));
    QVERIFY(layout.leftFloating());
    QCOMPARE(layout.centerColumnWidths().at(0).toInt(), 400);

    WorkspaceLayout restored;
    QVERIFY(restored.restoreFromJson(layout.toJson()));
    QCOMPARE(tilePlacements(restored), tilePlacements(layout));
}

void WorkspaceLayoutTest::newColumnSplitsEquallyWhenNeverDragged()
{
    WorkspaceLayout layout;
    addCenterTerminal(layout);
    addCenterTerminal(layout);
    addCenterTerminal(layout);
    QVERIFY(layout.setLeftWidth(250, 1600));
    QCOMPARE(layout.centerColumnWidths().size(), 2);
    const int first = layout.centerColumnWidths().at(0).toInt();
    const int second = layout.centerColumnWidths().at(1).toInt();
    QVERIFY(qAbs(first - second) <= 1);
    addCenterTerminal(layout);
    addCenterTerminal(layout);
    addCenterTerminal(layout);
    QCOMPARE(layout.centerColumnWidths().size(), 3);
    QCOMPARE(occupiedCenterWidth(layout), 1350);
    const int a = layout.centerColumnWidths().at(0).toInt();
    const int b = layout.centerColumnWidths().at(1).toInt();
    const int c = layout.centerColumnWidths().at(2).toInt();
    QVERIFY(qAbs(a - b) <= 2);
    QVERIFY(qAbs(b - c) <= 2);
}

void WorkspaceLayoutTest::addedColumnKeepsDraggedWidthRatios()
{
    WorkspaceLayout layout;
    addCenterTerminal(layout);
    addCenterTerminal(layout);
    addCenterTerminal(layout);
    QVERIFY(layout.setLeftWidth(250, 2000));
    const int startLeft = layout.centerColumnWidths().at(0).toInt();
    QVERIFY(layout.setCenterColumnWidth(0, startLeft - 120));
    const int small = layout.centerColumnWidths().at(0).toInt();
    const int large = layout.centerColumnWidths().at(1).toInt();
    QVERIFY(small < large);
    const int oldSum = small + large;
    addCenterTerminal(layout);
    addCenterTerminal(layout);
    addCenterTerminal(layout);
    QCOMPARE(layout.centerColumnWidths().size(), 3);
    QCOMPARE(occupiedCenterWidth(layout), 1750);
    const int available = 1750;
    const int addedWeight = oldSum / 2;
    const int weightSum = oldSum + addedWeight;
    const int expectedSmall = small * available / weightSum;
    const int expectedLarge = large * available / weightSum;
    QVERIFY(layout.centerColumnWidths().at(0).toInt() < layout.centerColumnWidths().at(1).toInt());
    QVERIFY(qAbs(layout.centerColumnWidths().at(0).toInt() - expectedSmall) <= 3);
    QVERIFY(qAbs(layout.centerColumnWidths().at(1).toInt() - expectedLarge) <= 3);
}

void WorkspaceLayoutTest::insertColumnShiftsExisting()
{
    WorkspaceLayout layout;
    const QString terminal = addCenterTerminal(layout);
    const QString editor = layout.addProjectTile(QStringLiteral("editor"),
                                                 QStringLiteral("C:/work"));
    QVERIFY(layout.insertCenterTile(editor, 1, WorkspaceLayout::DropColumn));
    QCOMPARE(roleString(layout, editor, WorkspaceLayout::ZoneRole), QStringLiteral("center"));
    QCOMPARE(roleInt(layout, editor, WorkspaceLayout::ColumnRole), 1);
    QCOMPARE(roleInt(layout, editor, WorkspaceLayout::RowSpanRole), 2);
    QCOMPARE(roleInt(layout, terminal, WorkspaceLayout::ColumnRole), 0);
    QCOMPARE(roleInt(layout, terminal, WorkspaceLayout::RowSpanRole), 2);
    QCOMPARE(layout.centerColumnWidths().size(), 2);
}

void WorkspaceLayoutTest::insertUpperSplitsFullColumn()
{
    WorkspaceLayout layout;
    const QString first = addCenterTerminal(layout);
    const QString second = addCenterTerminal(layout);
    QVERIFY(layout.insertCenterTile(second, 1, WorkspaceLayout::DropColumn));
    QCOMPARE(roleInt(layout, first, WorkspaceLayout::RowSpanRole), 2);
    QCOMPARE(roleInt(layout, second, WorkspaceLayout::RowSpanRole), 2);
    QVERIFY(layout.insertCenterTile(second, 0, WorkspaceLayout::DropUpper));
    QCOMPARE(roleInt(layout, second, WorkspaceLayout::ColumnRole), 0);
    QCOMPARE(roleInt(layout, second, WorkspaceLayout::RowRole), 0);
    QCOMPARE(roleInt(layout, second, WorkspaceLayout::RowSpanRole), 1);
    QCOMPARE(roleInt(layout, first, WorkspaceLayout::ColumnRole), 0);
    QCOMPARE(roleInt(layout, first, WorkspaceLayout::RowRole), 1);
    QCOMPARE(roleInt(layout, first, WorkspaceLayout::RowSpanRole), 1);
    QCOMPARE(layout.centerColumnWidths().size(), 1);
}

void WorkspaceLayoutTest::insertLowerPutsTileUnderFullColumn()
{
    WorkspaceLayout layout;
    const QString first = addCenterTerminal(layout);
    const QString second = addCenterTerminal(layout);
    QVERIFY(layout.insertCenterTile(second, 1, WorkspaceLayout::DropColumn));
    QVERIFY(layout.insertCenterTile(second, 0, WorkspaceLayout::DropLower));
    QCOMPARE(roleInt(layout, first, WorkspaceLayout::RowRole), 0);
    QCOMPARE(roleInt(layout, first, WorkspaceLayout::RowSpanRole), 1);
    QCOMPARE(roleInt(layout, second, WorkspaceLayout::RowRole), 1);
    QCOMPARE(roleInt(layout, second, WorkspaceLayout::RowSpanRole), 1);
    QCOMPARE(roleInt(layout, second, WorkspaceLayout::ColumnRole), 0);
}

void WorkspaceLayoutTest::bottomSmallMovesToUpperInSameColumn()
{
    WorkspaceLayout layout;
    const QString top = addCenterTerminal(layout);
    const QString bottom = addCenterTerminal(layout);
    QCOMPARE(roleInt(layout, top, WorkspaceLayout::RowRole), 0);
    QCOMPARE(roleInt(layout, bottom, WorkspaceLayout::RowRole), 1);
    QVERIFY(layout.insertCenterTile(bottom, 0, WorkspaceLayout::DropUpper));
    QCOMPARE(roleInt(layout, bottom, WorkspaceLayout::RowRole), 0);
    QCOMPARE(roleInt(layout, bottom, WorkspaceLayout::RowSpanRole), 1);
    QCOMPARE(roleInt(layout, top, WorkspaceLayout::RowRole), 1);
    QCOMPARE(roleInt(layout, top, WorkspaceLayout::RowSpanRole), 1);
}

void WorkspaceLayoutTest::insertUpperIntoTwoTileColumnDisplaces()
{
    WorkspaceLayout layout;
    const QString top = addCenterTerminal(layout);
    const QString bottom = addCenterTerminal(layout);
    const QString extra = addCenterTerminal(layout);
    // A third tile already opens its own full-height column.
    QCOMPARE(roleInt(layout, extra, WorkspaceLayout::ColumnRole), 1);
    QCOMPARE(roleInt(layout, extra, WorkspaceLayout::RowSpanRole), 2);
    QVERIFY(layout.insertCenterTile(extra, 0, WorkspaceLayout::DropUpper));
    QCOMPARE(roleInt(layout, extra, WorkspaceLayout::ColumnRole), 0);
    QCOMPARE(roleInt(layout, extra, WorkspaceLayout::RowRole), 0);
    QCOMPARE(roleInt(layout, extra, WorkspaceLayout::RowSpanRole), 1);
    QCOMPARE(roleInt(layout, bottom, WorkspaceLayout::ColumnRole), 0);
    QCOMPARE(roleInt(layout, bottom, WorkspaceLayout::RowRole), 1);
    QCOMPARE(roleInt(layout, top, WorkspaceLayout::ColumnRole), 1);
    QCOMPARE(roleInt(layout, top, WorkspaceLayout::RowSpanRole), 2);
}

void WorkspaceLayoutTest::previewMatchesCommittedInsert()
{
    WorkspaceLayout layout;
    const QString first = addCenterTerminal(layout);
    const QString second = addCenterTerminal(layout);
    QVERIFY(layout.insertCenterTile(second, 1, WorkspaceLayout::DropColumn));
    const QVariantMap preview = layout.previewCenterInsert(
        second, 0, WorkspaceLayout::DropUpper);
    QVERIFY(preview.value(QStringLiteral("ok")).toBool());
    QVERIFY(layout.insertCenterTile(second, 0, WorkspaceLayout::DropUpper));
    QCOMPARE(roleInt(layout, second, WorkspaceLayout::ColumnRole),
             preview.value(QStringLiteral("column")).toInt());
    QCOMPARE(roleInt(layout, second, WorkspaceLayout::RowRole),
             preview.value(QStringLiteral("row")).toInt());
    QCOMPARE(roleInt(layout, second, WorkspaceLayout::RowSpanRole),
             preview.value(QStringLiteral("rowSpan")).toInt());
    QCOMPARE(layout.centerColumnWidths(),
             preview.value(QStringLiteral("columnWidths")).toList());
    Q_UNUSED(first);
}

void WorkspaceLayoutTest::dropOnOwnFullColumnIsNoOp()
{
    WorkspaceLayout layout;
    const QString first = addCenterTerminal(layout);
    const QString second = addCenterTerminal(layout);
    QVERIFY(layout.insertCenterTile(second, 1, WorkspaceLayout::DropColumn));
    const QVariantList widths = layout.centerColumnWidths();
    QSignalSpy tilesChanged(&layout, &WorkspaceLayout::tilesChanged);

    QCOMPARE(acceptedDropSlots(layout, first, 0,
                               {WorkspaceLayout::DropColumn, WorkspaceLayout::DropUpper, WorkspaceLayout::DropLower,
                                WorkspaceLayout::DropLeft, WorkspaceLayout::DropRight}),
             QList<int>{});
    // Left edge of the neighbour column is the same seam as the own right edge.
    QVERIFY(!layout.insertCenterTile(first, 1, WorkspaceLayout::DropLeft));

    QCOMPARE(tilesChanged.count(), 0);
    QCOMPARE(roleInt(layout, first, WorkspaceLayout::ColumnRole), 0);
    QCOMPARE(roleInt(layout, first, WorkspaceLayout::RowSpanRole), 2);
    QCOMPARE(roleInt(layout, second, WorkspaceLayout::ColumnRole), 1);
    QCOMPARE(roleInt(layout, second, WorkspaceLayout::RowSpanRole), 2);
    QCOMPARE(layout.centerColumnWidths(), widths);
}

void WorkspaceLayoutTest::dropOnOwnStackedSlotIsNoOp()
{
    WorkspaceLayout layout;
    const QString top = addCenterTerminal(layout);
    const QString bottom = addCenterTerminal(layout);
    QSignalSpy tilesChanged(&layout, &WorkspaceLayout::tilesChanged);
    QVERIFY(!layout.insertCenterTile(top, 0, WorkspaceLayout::DropUpper));
    QVERIFY(!layout.insertCenterTile(bottom, 0, WorkspaceLayout::DropLower));
    QCOMPARE(tilesChanged.count(), 0);
    QCOMPARE(roleInt(layout, top, WorkspaceLayout::RowRole), 0);
    QCOMPARE(roleInt(layout, bottom, WorkspaceLayout::RowRole), 1);
}

void WorkspaceLayoutTest::dropLowerOnOwnStackedColumnSwaps()
{
    WorkspaceLayout layout;
    const QString top = addCenterTerminal(layout);
    const QString bottom = addCenterTerminal(layout);
    QVERIFY(layout.insertCenterTile(top, 0, WorkspaceLayout::DropLower));
    QCOMPARE(roleInt(layout, top, WorkspaceLayout::RowRole), 1);
    QCOMPARE(roleInt(layout, bottom, WorkspaceLayout::RowRole), 0);
    QCOMPARE(roleInt(layout, top, WorkspaceLayout::ColumnRole), 0);
    QCOMPARE(roleInt(layout, bottom, WorkspaceLayout::ColumnRole), 0);
}

void WorkspaceLayoutTest::insertCenterRejectsMissingAndProjects()
{
    WorkspaceLayout layout;
    const QString projects = layout.data(layout.index(0, 0), WorkspaceLayout::IdRole).toString();
    QVERIFY(!layout.insertCenterTile(QStringLiteral("missing"), 0, WorkspaceLayout::DropColumn));
    QVERIFY(!layout.insertCenterTile(projects, 0, WorkspaceLayout::DropColumn));
    QCOMPARE(roleString(layout, projects, WorkspaceLayout::ZoneRole), QStringLiteral("left"));
}

void WorkspaceLayoutTest::dropSlotAtUsesEdgeColumnsAndVerticalHalves()
{
    WorkspaceLayout layout;
    QCOMPARE(layout.dropSlotAt(0.0, 0.5), WorkspaceLayout::DropLeft);
    QCOMPARE(layout.dropSlotAt(0.17, 0.5), WorkspaceLayout::DropLeft);
    QCOMPARE(layout.dropSlotAt(0.18, 0.0), WorkspaceLayout::DropUpper);
    QCOMPARE(layout.dropSlotAt(0.5, 0.49), WorkspaceLayout::DropUpper);
    QCOMPARE(layout.dropSlotAt(0.5, 0.5), WorkspaceLayout::DropLower);
    QCOMPARE(layout.dropSlotAt(0.82, 0.5), WorkspaceLayout::DropLower);
    QCOMPARE(layout.dropSlotAt(0.83, 0.5), WorkspaceLayout::DropRight);
    QCOMPARE(layout.dropSlotAt(1.0, 0.5), WorkspaceLayout::DropRight);
}


void WorkspaceLayoutTest::roundTripsJsonWithTwoByTwo()
{
    WorkspaceLayout layout;
    QStringList ids;
    fillTwoByTwo(layout, &ids);
    QVERIFY(layout.setLeftWidth(280, 1200));
    QVERIFY(layout.setCenterRowRatio(0, 0.4));
    QVERIFY(layout.setCenterRowRatio(1, 0.35));
    const QString json = layout.toJson();
    const QJsonObject root = QJsonDocument::fromJson(json.toUtf8()).object();
    QCOMPARE(root.value(QStringLiteral("v")).toInt(), 4);
    QVERIFY(root.value(QStringLiteral("tiles")).isArray());
    QVERIFY(root.value(QStringLiteral("rowRatios")).isArray());

    WorkspaceLayout restored;
    QVERIFY(restored.restoreFromJson(json));
    // The window width is not stored; the restored layout gets the same one.
    QVERIFY(restored.previewViewport(1200, kDefaultCenterHeight));
    restored.commitViewport();
    QCOMPARE(restored.rowCount(), layout.rowCount());
    QCOMPARE(restored.leftWidth(), layout.leftWidth());
    QCOMPARE(restored.centerRowRatio(0), layout.centerRowRatio(0));
    QCOMPARE(restored.centerRowRatio(1), layout.centerRowRatio(1));
    QCOMPARE(restored.centerColumnWidths().size(), layout.centerColumnWidths().size());
    QCOMPARE(tilePlacements(restored), tilePlacements(layout));
}

void WorkspaceLayoutTest::restoreRejectsMalformedJson()
{
    WorkspaceLayout layout;
    const QString editor = layout.addProjectTile(QStringLiteral("editor"),
                                                 QStringLiteral("C:/work"));
    QCOMPARE(layout.rowCount(), 2);
    QVERIFY(!layout.restoreFromJson(QStringLiteral("not-json")));
    QCOMPARE(layout.rowCount(), 2);
    QCOMPARE(roleString(layout, editor, WorkspaceLayout::KindRole), QStringLiteral("editor"));
    QVERIFY(!layout.restoreFromJson(QStringLiteral("{\"v\":4,\"tiles\":[{}]}")));
    QCOMPARE(layout.rowCount(), 2);
}


void WorkspaceLayoutTest::clampedColumnWidthDoesNotMutate()
{
    WorkspaceLayout layout;
    addCenterTerminal(layout);
    addCenterTerminal(layout);
    addCenterTerminal(layout);
    QVERIFY(layout.setLeftWidth(250, 1600));
    const int beforeLeft = layout.centerColumnWidths().at(0).toInt();
    const int beforeRight = layout.centerColumnWidths().at(1).toInt();
    const int preview = layout.clampedCenterColumnWidth(0, beforeLeft + 80);
    QCOMPARE(preview, beforeLeft + 80);
    QCOMPARE(layout.centerColumnWidths().at(0).toInt(), beforeLeft);
    QCOMPARE(layout.centerColumnWidths().at(1).toInt(), beforeRight);
    QVERIFY(layout.setCenterColumnWidth(0, preview));
    QCOMPARE(layout.centerColumnWidths().at(0).toInt(), preview);
    QCOMPARE(layout.centerColumnWidths().at(1).toInt(), beforeRight - 80);
}

void WorkspaceLayoutTest::stackedColumnReportsTwoRows()
{
    WorkspaceLayout layout;
    addCenterTerminal(layout);
    QVERIFY(!layout.centerColumnStacked(0));
    addCenterTerminal(layout);
    QVERIFY(layout.centerColumnStacked(0));
    QVERIFY(!layout.centerColumnStacked(1));
    addCenterTerminal(layout);
    QVERIFY(layout.centerColumnStacked(0));
    QVERIFY(!layout.centerColumnStacked(1));
}

void WorkspaceLayoutTest::clampsRowRatioToMinHeight()
{
    WorkspaceLayout layout;
    addCenterTerminal(layout);
    addCenterTerminal(layout);
    QVERIFY(layout.setCenterRowRatio(0, 0.1));
    QCOMPARE(layout.centerRowRatio(0), qreal(kMinRowHeight) / 640.0);
    QCOMPARE(layout.clampedCenterRowRatio(0.5), 0.5);
}

void WorkspaceLayoutTest::rowSplitIsIndependentPerColumn()
{
    WorkspaceLayout layout;
    QStringList ids;
    fillTwoByTwo(layout, &ids);
    QCOMPARE(layout.centerRowRatio(0), 0.5);
    QCOMPARE(layout.centerRowRatio(1), 0.5);
    QVERIFY(layout.setCenterRowRatio(0, 0.3));
    QCOMPARE(layout.centerRowRatio(0), 0.3);
    QCOMPARE(layout.centerRowRatio(1), 0.5);
    QVERIFY(layout.setCenterRowRatio(1, 0.7));
    QCOMPARE(layout.centerRowRatio(0), 0.3);
    QCOMPARE(layout.centerRowRatio(1), 0.7);
}

void WorkspaceLayoutTest::viewportPreviewIsTransientUntilCommit()
{
    WorkspaceLayout layout;
    addCenterTerminal(layout);
    addCenterTerminal(layout);
    addCenterTerminal(layout);
    QVERIFY(layout.setLeftWidth(260, 2200));
    QSignalSpy geometry(&layout, &WorkspaceLayout::geometryChanged);
    QSignalSpy committed(&layout, &WorkspaceLayout::layoutCommitted);
    const QJsonObject durableBefore =
        QJsonDocument::fromJson(layout.toJson().toUtf8()).object();

    QVERIFY(layout.previewViewport(1800, 700));
    QVERIFY(layout.previewViewport(2200, 760));
    QVERIFY(geometry.count() >= 1);
    QCOMPARE(committed.count(), 0);
    const QJsonObject duringPreview =
        QJsonDocument::fromJson(layout.toJson().toUtf8()).object();
    QCOMPARE(duringPreview.value(QStringLiteral("columnWidths")),
             durableBefore.value(QStringLiteral("columnWidths")));
    layout.commitViewport();
    QCOMPARE(committed.count(), 1);
    // A second commit with nothing new previewed persists nothing.
    layout.commitViewport();
    QCOMPARE(committed.count(), 1);

    const QJsonObject root =
        QJsonDocument::fromJson(layout.toJson().toUtf8()).object();
    QCOMPARE(root.value(QStringLiteral("v")).toInt(), 4);
    QVERIFY(!root.contains(QStringLiteral("viewport")));
    QVERIFY(root.value(QStringLiteral("columnWidths")).toArray().size() >= 1);
}

void WorkspaceLayoutTest::paneSeamSurvivesSameSizePreviewDuringTransaction()
{
    WorkspaceLayout layout;
    addCenterTerminal(layout);
    addCenterTerminal(layout);
    addCenterTerminal(layout);
    QVERIFY(layout.setLeftWidth(250, 2000));
    QVERIFY(layout.previewViewport(2000, 700));
    const int beforeSeam = layout.centerColumnWidths().at(0).toInt();
    QVERIFY(beforeSeam >= kMinCenterWidth);
    const int desired = beforeSeam + 80;
    QVERIFY(layout.setCenterColumnWidth(0, desired));
    const int afterSeam = layout.centerColumnWidths().at(0).toInt();
    QVERIFY(afterSeam != beforeSeam);
    QVERIFY(layout.previewViewport(2000, 700));
    QCOMPARE(layout.centerColumnWidths().at(0).toInt(), afterSeam);
    layout.commitViewport();
    const QJsonObject root =
        QJsonDocument::fromJson(layout.toJson().toUtf8()).object();
    QCOMPARE(root.value(QStringLiteral("columnWidths")).toArray().at(0).toInt(),
             afterSeam);
}

void WorkspaceLayoutTest::dropRightAtRightmostEdgeDoesNotCreateGhostColumn()
{
    WorkspaceLayout layout;
    const QString first = addCenterTerminal(layout);
    const QString second = addCenterTerminal(layout);
    const QString third = addCenterTerminal(layout);
    QCOMPARE(layout.centerColumnWidths().size(), 2);
    QVERIFY(layout.insertCenterTile(first, 2, WorkspaceLayout::DropRight));
    QVERIFY(centerColumnsContiguous(layout, int(layout.centerColumnWidths().size())));
}

void WorkspaceLayoutTest::dropBesideColumnSplitsItEvenly()
{
    WorkspaceLayout layout;
    addCenterTerminal(layout);
    const QString lower = addCenterTerminal(layout);
    QVERIFY(layout.setLeftWidth(250, 1600));
    QCOMPARE(layout.centerColumnWidths().size(), 1);
    QCOMPARE(roleInt(layout, lower, WorkspaceLayout::RowRole), 1);

    // The lower tile of the stack dragged to the right edge of its column.
    QVERIFY(layout.insertCenterTile(lower, 0, WorkspaceLayout::DropRight));
    QCOMPARE(roleInt(layout, lower, WorkspaceLayout::ColumnRole), 1);
    const QVariantList widths = layout.centerColumnWidths();
    QCOMPARE(widths.size(), 2);
    QCOMPARE(occupiedCenterWidth(layout), 1350);
    QVERIFY2(qAbs(widths.at(0).toInt() - widths.at(1).toInt()) <= 1,
             qPrintable(QStringLiteral("%1 vs %2").arg(widths.at(0).toInt()).arg(widths.at(1).toInt())));
}

void WorkspaceLayoutTest::dropBesideColumnKeepsOtherColumnWidths()
{
    WorkspaceLayout layout;
    QStringList ids;
    fillTwoByTwo(layout, &ids);
    QVERIFY(layout.setLeftWidth(250, 2000));
    // Column 0 narrow, column 1 wide.
    QVERIFY(layout.setCenterColumnWidth(0, 650));
    const int wide = layout.centerColumnWidths().at(1).toInt();
    QCOMPARE(wide, 1750 - 650);

    // A tile of column 0 opens a column left of column 1: column 1 gives
    // half its width to it, column 0 keeps its own.
    const QString moving = ids.at(0);
    QCOMPARE(roleInt(layout, moving, WorkspaceLayout::ColumnRole), 0);
    QVERIFY(layout.insertCenterTile(moving, 1, WorkspaceLayout::DropLeft));
    const QVariantList widths = layout.centerColumnWidths();
    QCOMPARE(widths.size(), 3);
    QCOMPARE(roleInt(layout, moving, WorkspaceLayout::ColumnRole), 1);
    QCOMPARE(widths.at(0).toInt(), 650);
    QVERIFY(qAbs(widths.at(1).toInt() - widths.at(2).toInt()) <= 1);
    QCOMPARE(occupiedCenterWidth(layout), 1750);
}

void WorkspaceLayoutTest::clampedSideWidthDoesNotExceedWindowLimit()
{
    WorkspaceLayout layout;
    addCenterTerminal(layout);
    QVERIFY(layout.setLeftWidth(5000, 1000));
    QVERIFY(layout.leftWidth() <= 1000 - kMinCenterWidth);
    QVERIFY(layout.leftWidth() >= kMinLeftWidth);

    // The right side is held to its minimum; it still docks, and the left
    // one gives way to make room for it.
    QVERIFY(layout.setRightWidth(5000, 1000));
    QCOMPARE(layout.rightPanelWidth(), kMinRightWidth);
    QVERIFY(!layout.rightFloating());
    QCOMPARE(layout.rightWidth(), kMinRightWidth);
    QCOMPARE(layout.leftWidth(), 1000 - kMinRightWidth - kMinCenterWidth);
    QCOMPARE(layout.clampedSideWidth(QStringLiteral("left"), 5000, 1000), layout.leftWidth());
    QCOMPARE(layout.clampedSideWidth(QStringLiteral("left"), 10, 1000), kSideCollapseWidth);
}

QTEST_MAIN(WorkspaceLayoutTest)
#include "workspace_layout_test.moc"
