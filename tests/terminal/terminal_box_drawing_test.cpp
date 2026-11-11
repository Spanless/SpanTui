#include "terminal/render/terminal_drawn_glyphs.h"

#include <QLineF>
#include <QTest>

#include <algorithm>

using namespace span::terminal;

namespace {

const QRectF kCell(10, 20, 8, 18);

qreal horizontalCenter(const QRectF &cell, qreal ratio) {
    const TerminalBoxDrawingGeometry geometry =
        TerminalBoxDrawing::geometry(QStringLiteral("─"), cell, ratio);
    return geometry.strokes.constFirst().centerLine.constFirst().y();
}

qreal verticalCenter(const QRectF &cell, qreal ratio) {
    const TerminalBoxDrawingGeometry geometry =
        TerminalBoxDrawing::geometry(QStringLiteral("│"), cell, ratio);
    return geometry.strokes.constFirst().centerLine.constFirst().x();
}

bool nearlyEqual(qreal first, qreal second, qreal tolerance = 0.0001) {
    return qAbs(first - second) <= tolerance;
}

bool nearlyEqual(const QPointF &first, const QPointF &second)
{
    return nearlyEqual(first.x(), second.x()) && nearlyEqual(first.y(), second.y());
}

QPointF absolute(const QPointF &point)
{
    return QPointF(qAbs(point.x()), qAbs(point.y()));
}

QVector<QPointF> centerPoints(const TerminalBoxDrawingGeometry &geometry)
{
    QVector<QPointF> points;
    for (const TerminalStrokePath &stroke : geometry.strokes) points += stroke.centerLine;
    return points;
}

QPair<QPointF, QPointF> endpoints(const QString &glyph, qreal ratio)
{
    const TerminalStrokePath stroke = TerminalBoxDrawing::geometry(glyph, kCell, ratio).strokes.constFirst();
    return qMakePair(stroke.centerLine.first(), stroke.centerLine.last());
}

// The first index where `right` is not `left` mirrored across the cell's
// vertical centre, or -1.
qsizetype firstUnmirroredPoint(const QVector<QPointF> &left, const QVector<QPointF> &right)
{
    for (qsizetype index = 0; index < left.size(); ++index) {
        const QPointF mirrored(kCell.left() + kCell.right() - right.at(index).x(), right.at(index).y());
        if (!nearlyEqual(left.at(index), mirrored)) return index;
    }
    return -1;
}

qreal closestToDiagonal(const QVector<QPointF> &normals)
{
    qreal closest = 1.0;
    for (const QPointF &normal : normals) closest = std::min(closest, qAbs(qAbs(normal.x()) - qAbs(normal.y())));
    return closest;
}

// Coverage and distance from the quad's first vertex, for each vertex of
// quad `quad` (four vertices across the stroke).
QStringList quadProfile(const TerminalCoverageMesh &mesh, int quad)
{
    QStringList profile;
    const QPointF origin = mesh.vertices.at(quad * 4).position;
    for (int index = quad * 4; index < quad * 4 + 4; ++index) {
        profile.append(QStringLiteral("%1 at %2")
                           .arg(int(mesh.vertices.at(index).coverage))
                           .arg(QLineF(origin, mesh.vertices.at(index).position).length(), 0, 'f', 4));
    }
    return profile;
}

// Quads that are not transparent fringe, opaque core, opaque core,
// transparent fringe, each step one device pixel apart.
QList<int> quadsOffProfile(const TerminalCoverageMesh &mesh, qreal ratio)
{
    QList<int> off;
    for (int index = 0; index < mesh.vertices.size(); index += 4) {
        const auto at = [&](int offset) { return mesh.vertices.at(index + offset); };
        const auto apart = [&](int from, int to) { return QLineF(at(from).position, at(to).position).length(); };
        const bool profile = at(0).coverage == 0 && at(1).coverage == 255 && at(2).coverage == 255
            && at(3).coverage == 0 && nearlyEqual(apart(1, 2), 1.0 / ratio) && nearlyEqual(apart(0, 1), 1.0 / ratio)
            && nearlyEqual(apart(2, 3), 1.0 / ratio) && nearlyEqual(apart(0, 3), 3.0 / ratio);
        if (!profile) off.append(index / 4);
    }
    return off;
}

bool indicesInRange(const TerminalCoverageMesh &mesh)
{
    return std::all_of(mesh.indices.cbegin(), mesh.indices.cend(),
                       [&](quint16 index) { return index < mesh.vertices.size(); });
}

} // namespace

class TerminalBoxDrawingTest final : public QObject {
    Q_OBJECT

private slots:
    void recognizesCommonTerminalGlyphs();
    void unsupportedTextProducesNoGeometry();
    void verticalStrokeTouchesBothCellEdges();
    void squareCornerJoinsItsTwoEdges();
    void preservesDashedAndJunctionGeometry();
    void usesFixedLightAndHeavyStrokes();
    void roundedCornersUseContinuousCurves_data();
    void roundedCornersUseContinuousCurves();
    void roundedCornersJoinNeighborCentersAtEveryDpi_data();
    void roundedCornersJoinNeighborCentersAtEveryDpi();
    void roundedCornersAreMirrored();
    void roundedCornersUseACompactCircularBend();
    void straightAndRoundedLinesUseTheSameCoverage();
    void strokeMeshHasStableCoreAndAntialiasFringe_data();
    void strokeMeshHasStableCoreAndAntialiasFringe();
    void strokeMeshRejectsDegeneratePaths();
};

void TerminalBoxDrawingTest::recognizesCommonTerminalGlyphs() {
    QVERIFY(TerminalBoxDrawing::supports(QStringLiteral("│")));
    QVERIFY(TerminalBoxDrawing::supports(QStringLiteral("┌")));
    QVERIFY(TerminalBoxDrawing::supports(QStringLiteral("┼")));
    QVERIFY(TerminalBoxDrawing::supports(QStringLiteral("╭")));
    QVERIFY(TerminalBoxDrawing::supports(QStringLiteral("╮")));
    QVERIFY(TerminalBoxDrawing::supports(QStringLiteral("╯")));
    QVERIFY(TerminalBoxDrawing::supports(QStringLiteral("╰")));
    QVERIFY(!TerminalBoxDrawing::supports(QStringLiteral("A")));
}

void TerminalBoxDrawingTest::unsupportedTextProducesNoGeometry() {
    QVERIFY(TerminalBoxDrawing::geometry(QStringLiteral("A"), kCell, 1.0).isEmpty());
    QVERIFY(TerminalBoxDrawing::geometry(QStringLiteral("ab"), kCell, 1.0).isEmpty());
    QVERIFY(TerminalBoxDrawing::geometry(QString(), kCell, 1.0).isEmpty());
}

void TerminalBoxDrawingTest::verticalStrokeTouchesBothCellEdges() {
    const TerminalBoxDrawingGeometry geometry =
        TerminalBoxDrawing::geometry(QStringLiteral("│"), kCell, 1.0);

    QCOMPARE(geometry.strokes.size(), 1);
    const TerminalStrokePath &stroke = geometry.strokes.constFirst();
    QCOMPARE(stroke.centerLine.size(), 2);
    QCOMPARE(stroke.centerLine.first().y(), kCell.top());
    QCOMPARE(stroke.centerLine.last().y(), kCell.bottom());
}

void TerminalBoxDrawingTest::squareCornerJoinsItsTwoEdges() {
    const TerminalBoxDrawingGeometry geometry =
        TerminalBoxDrawing::geometry(QStringLiteral("┌"), kCell, 1.0);

    QCOMPARE(geometry.strokes.size(), 2);
    const QVector<QPointF> points = centerPoints(geometry);
    QVERIFY(
        std::any_of(points.cbegin(), points.cend(), [](const QPointF &point) { return point.x() == kCell.right(); }));
    QVERIFY(
        std::any_of(points.cbegin(), points.cend(), [](const QPointF &point) { return point.y() == kCell.bottom(); }));
}

void TerminalBoxDrawingTest::preservesDashedAndJunctionGeometry() {
    const TerminalBoxDrawingGeometry dashed =
        TerminalBoxDrawing::geometry(QStringLiteral("┄"), kCell, 1.0);
    const TerminalBoxDrawingGeometry junction =
        TerminalBoxDrawing::geometry(QStringLiteral("┼"), kCell, 1.0);

    QCOMPARE(dashed.strokes.size(), 3);
    QCOMPARE(junction.strokes.size(), 2);
}

void TerminalBoxDrawingTest::usesFixedLightAndHeavyStrokes() {
    const TerminalBoxDrawingGeometry light =
        TerminalBoxDrawing::geometry(QStringLiteral("─"), kCell, 1.0);
    const TerminalBoxDrawingGeometry heavy =
        TerminalBoxDrawing::geometry(QStringLiteral("━"), kCell, 1.0);
    const TerminalBoxDrawingGeometry highDpi =
        TerminalBoxDrawing::geometry(QStringLiteral("─"), kCell, 2.0);

    QCOMPARE(light.strokes.size(), 1);
    QCOMPARE(heavy.strokes.size(), 1);
    QCOMPARE(highDpi.strokes.size(), 1);
    QCOMPARE(light.strokes.first().width, 2.0);
    QCOMPARE(heavy.strokes.first().width, 3.0);
    // Widths are logical: two device pixels at any ratio.
    QCOMPARE(highDpi.strokes.first().width, 1.0);
}

void TerminalBoxDrawingTest::roundedCornersUseContinuousCurves_data() {
    QTest::addColumn<QString>("glyph");
    QTest::addColumn<QPointF>("firstEndpoint");
    QTest::addColumn<QPointF>("lastEndpoint");
    // Unsigned normals at each end: a horizontal end's normal is vertical.
    QTest::addColumn<QPointF>("firstNormal");
    QTest::addColumn<QPointF>("lastNormal");

    const qreal centerX = verticalCenter(kCell, 1.0);
    const qreal centerY = horizontalCenter(kCell, 1.0);
    const QPointF vertical(0, 1);
    const QPointF horizontal(1, 0);
    QTest::newRow("top-left") << QStringLiteral("╭") << QPointF(kCell.right(), centerY)
                              << QPointF(centerX, kCell.bottom()) << vertical << horizontal;
    QTest::newRow("top-right") << QStringLiteral("╮") << QPointF(kCell.left(), centerY)
                               << QPointF(centerX, kCell.bottom()) << vertical << horizontal;
    QTest::newRow("bottom-right") << QStringLiteral("╯") << QPointF(kCell.left(), centerY)
                                  << QPointF(centerX, kCell.top()) << vertical << horizontal;
    QTest::newRow("bottom-left") << QStringLiteral("╰") << QPointF(centerX, kCell.top())
                                 << QPointF(kCell.right(), centerY) << horizontal << vertical;
}

void TerminalBoxDrawingTest::roundedCornersUseContinuousCurves() {
    QFETCH(QString, glyph);
    QFETCH(QPointF, firstEndpoint);
    QFETCH(QPointF, lastEndpoint);
    QFETCH(QPointF, firstNormal);
    QFETCH(QPointF, lastNormal);

    const TerminalBoxDrawingGeometry geometry =
        TerminalBoxDrawing::geometry(glyph, kCell, 1.0);

    QCOMPARE(geometry.strokes.size(), 1);
    const TerminalStrokePath &stroke = geometry.strokes.constFirst();
    QCOMPARE(stroke.width, 2.0);
    QVERIFY(stroke.centerLine.size() >= 5);
    QCOMPARE(stroke.normals.size(), stroke.centerLine.size());
    QCOMPARE(stroke.centerLine.first(), firstEndpoint);
    QCOMPARE(stroke.centerLine.last(), lastEndpoint);

    QVERIFY(nearlyEqual(absolute(stroke.normals.first()), firstNormal));
    QVERIFY(nearlyEqual(absolute(stroke.normals.last()), lastNormal));

    const QPointF middle = stroke.centerLine.at(stroke.centerLine.size() / 2);
    QVERIFY(!nearlyEqual(middle.x(), firstEndpoint.x()));
    QVERIFY(!nearlyEqual(middle.y(), lastEndpoint.y()));
}

void TerminalBoxDrawingTest::roundedCornersJoinNeighborCentersAtEveryDpi_data() {
    QTest::addColumn<qreal>("ratio");

    QTest::newRow("1.0") << 1.0;
    QTest::newRow("1.25") << 1.25;
    QTest::newRow("1.5") << 1.5;
    QTest::newRow("2.0") << 2.0;
}

void TerminalBoxDrawingTest::roundedCornersJoinNeighborCentersAtEveryDpi() {
    QFETCH(qreal, ratio);

    const qreal centerX = verticalCenter(kCell, ratio);
    const qreal centerY = horizontalCenter(kCell, ratio);
    const TerminalStrokePath horizontal =
        TerminalBoxDrawing::geometry(QStringLiteral("─"), kCell, ratio)
            .strokes.constFirst();
    const TerminalStrokePath vertical =
        TerminalBoxDrawing::geometry(QStringLiteral("│"), kCell, ratio)
            .strokes.constFirst();
    const qreal left = horizontal.centerLine.first().x();
    const qreal right = horizontal.centerLine.last().x();
    const qreal top = vertical.centerLine.first().y();
    const qreal bottom = vertical.centerLine.last().y();
    QCOMPARE(endpoints(QStringLiteral("╭"), ratio), qMakePair(QPointF(right, centerY), QPointF(centerX, bottom)));
    QCOMPARE(endpoints(QStringLiteral("╮"), ratio), qMakePair(QPointF(left, centerY), QPointF(centerX, bottom)));
    QCOMPARE(endpoints(QStringLiteral("╯"), ratio), qMakePair(QPointF(left, centerY), QPointF(centerX, top)));
    QCOMPARE(endpoints(QStringLiteral("╰"), ratio), qMakePair(QPointF(centerX, top), QPointF(right, centerY)));
}

void TerminalBoxDrawingTest::roundedCornersAreMirrored() {
    const TerminalBoxDrawingGeometry topLeftGeometry =
        TerminalBoxDrawing::geometry(QStringLiteral("╭"), kCell, 1.0);
    const TerminalBoxDrawingGeometry topRightGeometry =
        TerminalBoxDrawing::geometry(QStringLiteral("╮"), kCell, 1.0);
    const QVector<QPointF> topLeft =
        topLeftGeometry.strokes.constFirst().centerLine;
    const QVector<QPointF> topRight =
        topRightGeometry.strokes.constFirst().centerLine;

    QCOMPARE(topLeft.size(), topRight.size());
    QCOMPARE(firstUnmirroredPoint(topLeft, topRight), qsizetype(-1));
}

void TerminalBoxDrawingTest::roundedCornersUseACompactCircularBend() {
    const TerminalBoxDrawingGeometry geometry =
        TerminalBoxDrawing::geometry(QStringLiteral("╭"), kCell, 1.0);
    const TerminalStrokePath &stroke = geometry.strokes.constFirst();

    QVERIFY(stroke.centerLine.size() >= 6);
    const QPointF arcEnd =
        stroke.centerLine.at(stroke.centerLine.size() - 2);
    QVERIFY2(arcEnd.y() < kCell.bottom() - 1.0,
             "a tall terminal cell needs a straight tail after a circular bend");
    QVERIFY(nearlyEqual(arcEnd.x(), verticalCenter(kCell, 1.0)));
    QVERIFY(closestToDiagonal(stroke.normals) < 0.001);
}

void TerminalBoxDrawingTest::straightAndRoundedLinesUseTheSameCoverage() {
    const TerminalBoxDrawingGeometry straightGeometry =
        TerminalBoxDrawing::geometry(QStringLiteral("─"), kCell, 1.0);
    const TerminalBoxDrawingGeometry roundedGeometry =
        TerminalBoxDrawing::geometry(QStringLiteral("╭"), kCell, 1.0);
    const TerminalStrokePath &straight = straightGeometry.strokes.constFirst();
    const TerminalStrokePath &rounded = roundedGeometry.strokes.constFirst();
    const TerminalCoverageMesh straightMesh = TerminalStrokeMeshBuilder::build(
        straight.centerLine, straight.normals, straight.width, 1.0);
    const TerminalCoverageMesh roundedMesh = TerminalStrokeMeshBuilder::build(
        rounded.centerLine, rounded.normals, rounded.width, 1.0);

    QCOMPARE(straight.width, rounded.width);
    QCOMPARE(straightMesh.fringeWidth, roundedMesh.fringeWidth);
    QCOMPARE(quadProfile(roundedMesh, 0), quadProfile(straightMesh, 0));
}

void TerminalBoxDrawingTest::strokeMeshHasStableCoreAndAntialiasFringe_data() {
    QTest::addColumn<qreal>("ratio");

    QTest::newRow("1.0") << 1.0;
    QTest::newRow("1.25") << 1.25;
    QTest::newRow("1.5") << 1.5;
    QTest::newRow("2.0") << 2.0;
}

void TerminalBoxDrawingTest::strokeMeshHasStableCoreAndAntialiasFringe() {
    QFETCH(qreal, ratio);

    const TerminalBoxDrawingGeometry geometry =
        TerminalBoxDrawing::geometry(QStringLiteral("╭"), kCell, ratio);
    const TerminalStrokePath &stroke = geometry.strokes.constFirst();
    const TerminalCoverageMesh mesh =
        TerminalStrokeMeshBuilder::build(
            stroke.centerLine, stroke.normals, stroke.width, ratio);

    QVERIFY(!mesh.isEmpty());
    QVERIFY(nearlyEqual(stroke.width * ratio, 2.0));
    QCOMPARE(mesh.vertices.size(), stroke.centerLine.size() * 4);
    QCOMPARE(mesh.indices.size(), (stroke.centerLine.size() - 1) * 18);
    QVERIFY(nearlyEqual(mesh.fringeWidth, 1.0 / ratio));
    QCOMPARE(quadsOffProfile(mesh, ratio), QList<int>());
    QVERIFY(indicesInRange(mesh));
}

void TerminalBoxDrawingTest::strokeMeshRejectsDegeneratePaths() {
    QVERIFY(TerminalStrokeMeshBuilder::build({}, {}, 2.0, 1.0).isEmpty());
    QVERIFY(TerminalStrokeMeshBuilder::build(
                {QPointF(0, 0)}, {QPointF(0, 1)}, 2.0, 1.0).isEmpty());
    QVERIFY(TerminalStrokeMeshBuilder::build(
                {QPointF(0, 0), QPointF(1, 0)},
                {QPointF(0, 1)}, 2.0, 1.0).isEmpty());
    QVERIFY(TerminalStrokeMeshBuilder::build(
                {QPointF(0, 0), QPointF(1, 0)},
                {QPointF(), QPointF()}, 2.0, 1.0).isEmpty());
    QVERIFY(TerminalStrokeMeshBuilder::build(
                {QPointF(0, 0), QPointF(1, 0)},
                {QPointF(0, 1), QPointF(0, 1)}, 0.0, 1.0).isEmpty());
}

QTEST_MAIN(TerminalBoxDrawingTest)

#include "terminal_box_drawing_test.moc"
