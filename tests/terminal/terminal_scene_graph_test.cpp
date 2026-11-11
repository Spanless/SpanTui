#include "terminal/render/terminal_scene_graph.h"

#include <QSGNode>
#include <QTest>

#include <memory>

using namespace span::terminal;

namespace {

QVector<std::shared_ptr<const TerminalPreparedRow>> preparedRows(int count)
{
    QVector<std::shared_ptr<const TerminalPreparedRow>> rows;
    for (int row = 0; row < count; ++row) rows.append(std::make_shared<TerminalPreparedRow>());
    return rows;
}

// The scene a view hands over, minus everything optional.
struct Scene {
    Frame frame;
    TerminalImeCompositionState ime;
    TerminalSceneGraph::PaintState state;
    QSGNode *root = nullptr;

    explicit Scene(const QVector<std::shared_ptr<const TerminalPreparedRow>> *rows)
    {
        frame.rows = int(rows->size());
        state.frame = &frame;
        state.rows = rows;
        state.imeComposition = &ime;
        state.cellWidth = 9;
        state.cellHeight = 20;
        state.itemWidth = 720;
    }
    ~Scene() { delete root; }

    // Rows whose nodes took new content in this sync.
    int sync()
    {
        TerminalSceneGraph::PaintResult result;
        root = TerminalSceneGraph::sync(root, state, result);
        return result.updatedRows;
    }
};

} // namespace

class TerminalSceneGraphTest final : public QObject {
    Q_OBJECT
private slots:
    void graphicRectUsesEnginePixels();
    void fractionalCellsScaleAndKeepNegativeAnchor();
    void rowNodesFollowPreparedRowsNotTheirContent();
    void newSceneGenerationRebuildsEveryRow();
};

void TerminalSceneGraphTest::graphicRectUsesEnginePixels()
{
    // libghostty sizes the placement; whole-pixel cells draw it as given.
    GraphicPlacement placement;
    placement.column = 3;
    placement.row = 2;
    placement.cellOffset = QPoint(4, 5);
    placement.pixelSize = QSize(120, 60);
    QCOMPARE(TerminalSceneGraph::graphicRect(placement, 9, 21), QRectF(31, 47, 120, 60));

    // Nothing to draw without a size.
    placement.pixelSize = QSize();
    QVERIFY(TerminalSceneGraph::graphicRect(placement, 9, 21).isEmpty());
}

void TerminalSceneGraphTest::fractionalCellsScaleAndKeepNegativeAnchor()
{
    // The engine saw 10x20 cells (the rounded 9.5x20 the view draws): the
    // image keeps its cell span instead of drifting by half a pixel a cell.
    GraphicPlacement placement;
    placement.column = 2;
    placement.row = -1;
    placement.cellOffset = QPoint(10, 0);
    placement.pixelSize = QSize(40, 40);
    const QRectF rect = TerminalSceneGraph::graphicRect(placement, 9.5, 20);
    QCOMPARE(rect, QRectF(19 + 9.5, -20, 38, 40));
    QCOMPARE(rect.intersected(QRectF(0, 0, 800, 320)), QRectF(28.5, 0, 38, 20));
}

void TerminalSceneGraphTest::rowNodesFollowPreparedRowsNotTheirContent()
{
    // Another session (or a restarted one) prepares its own rows, even when
    // their content and revisions equal the rows on screen; a node keeps its
    // content only for the very row it shows.
    const auto first = preparedRows(3);
    const auto second = preparedRows(3);
    Scene scene(&first);
    QCOMPARE(scene.sync(), 3);
    QCOMPARE(scene.sync(), 0);
    scene.state.rows = &second;
    QCOMPARE(scene.sync(), 3);
    scene.state.rows = &first;
    QCOMPARE(scene.sync(), 3);
}

void TerminalSceneGraphTest::newSceneGenerationRebuildsEveryRow()
{
    const auto rows = preparedRows(2);
    Scene scene(&rows);
    QCOMPARE(scene.sync(), 2);
    scene.state.sceneGeneration = 1;
    QCOMPARE(scene.sync(), 2);
    QCOMPARE(scene.sync(), 0);
}

QTEST_MAIN(TerminalSceneGraphTest)
#include "terminal_scene_graph_test.moc"
