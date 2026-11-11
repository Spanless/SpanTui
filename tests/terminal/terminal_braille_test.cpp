#include "terminal/render/terminal_drawn_glyphs.h"
#include "terminal/render/terminal_font_set.h"

#include <QLineF>
#include <QTest>

#include <algorithm>
#include <cmath>

using namespace span::terminal;

namespace {

bool nearlyEqual(qreal left, qreal right, qreal tolerance = 0.001) {
    return qAbs(left - right) <= tolerance;
}

bool allInside(const QVector<QPointF> &points, const QRectF &cell)
{
    return std::all_of(points.cbegin(), points.cend(), [&](const QPointF &point) { return cell.contains(point); });
}

bool indicesInRange(const TerminalCoverageMesh &mesh)
{
    return std::all_of(mesh.indices.cbegin(), mesh.indices.cend(),
                       [&](quint16 index) { return index < mesh.vertices.size(); });
}

// Dots whose mesh is not an opaque centre, an opaque inner ring of radius 1
// and a transparent outer ring of radius 1.5, each ring point-symmetric.
QList<int> dotsOffRing(const TerminalCoverageMesh &mesh, const TerminalBrailleGeometry &geometry, int segments)
{
    const int verticesPerDot = 1 + segments * 2;
    QList<int> off;
    for (int dot = 0; dot < geometry.dotCenters.size(); ++dot) {
        const int base = dot * verticesPerDot;
        const QPointF center = geometry.dotCenters.at(dot);
        bool ring = mesh.vertices.at(base).position == center && mesh.vertices.at(base).coverage == 255;
        for (int segment = 0; segment < segments; ++segment) {
            const TerminalCoverageMeshVertex &inner = mesh.vertices.at(base + 1 + segment);
            const TerminalCoverageMeshVertex &outer = mesh.vertices.at(base + 1 + segments + segment);
            const QPointF mirrored = mesh.vertices.at(base + 1 + (segment + segments / 2) % segments).position;
            ring = ring && inner.coverage == 255 && outer.coverage == 0
                && nearlyEqual(QLineF(center, inner.position).length(), 1.0)
                && nearlyEqual(QLineF(center, outer.position).length(), 1.5)
                && nearlyEqual(inner.position.x() + mirrored.x(), center.x() * 2.0)
                && nearlyEqual(inner.position.y() + mirrored.y(), center.y() * 2.0);
        }
        if (!ring) off.append(dot);
    }
    return off;
}

// Centres whose 2x2 core leaves `cell`; touching its bottom-right corner
// is inside.
QVector<QPointF> coresOutside(const QVector<QPointF> &centers, const QRectF &cell)
{
    QVector<QPointF> outside;
    for (const QPointF &center : centers) {
        const QRectF core(center.x() - 1.0, center.y() - 1.0, 2.0, 2.0);
        const bool inside = cell.contains(core.topLeft())
            && (cell.contains(core.bottomRight()) || core.bottomRight() == cell.bottomRight());
        if (!inside) outside.append(center);
    }
    return outside;
}

} // namespace

class TerminalBrailleTest final : public QObject {
    Q_OBJECT

private slots:
    void recognizesBrailleRange();
    void mapsAllEightDots();
    void mapsUnicodeDotOrder();
    void extendsOneLatticeAcrossAdjacentCells();
    void keepsDiagonalCollinearAcrossCellRows();
    void geometryTracksFontCellsAcrossDprAndSizes_data();
    void geometryTracksFontCellsAcrossDprAndSizes();
    void buildsClosedCoverageMesh();
    void mapsCommonSpinnerFrames_data();
    void mapsCommonSpinnerFrames();
    void handlesEmptyAndMinimumGeometry();
};

void TerminalBrailleTest::recognizesBrailleRange() {
    QVERIFY(TerminalBraille::supports(QStringLiteral("⠀")));
    QVERIFY(TerminalBraille::supports(QStringLiteral("⣿")));
    QVERIFY(!TerminalBraille::supports(QStringLiteral("A")));
    QVERIFY(!TerminalBraille::supports(QStringLiteral("⠋⠙")));
}

void TerminalBrailleTest::mapsAllEightDots() {
    const QRectF cell(0, 0, 8, 20);
    const TerminalBrailleGeometry geometry =
        TerminalBraille::geometry(QStringLiteral("⣿"), cell, 1.0);

    QCOMPARE(geometry.dotCenters.size(), 8);
    QCOMPARE(geometry.coreDiameter, 2.0);
    QVERIFY(allInside(geometry.dotCenters, cell));
}

void TerminalBrailleTest::mapsUnicodeDotOrder() {
    const QRectF cell(0, 0, 8, 20);
    const TerminalBrailleGeometry leftTop =
        TerminalBraille::geometry(QStringLiteral("⠁"), cell, 1.0);
    const TerminalBrailleGeometry rightTop =
        TerminalBraille::geometry(QStringLiteral("⠈"), cell, 1.0);
    const TerminalBrailleGeometry leftBottom =
        TerminalBraille::geometry(QStringLiteral("⡀"), cell, 1.0);

    QCOMPARE(leftTop.dotCenters.size(), 1);
    QCOMPARE(rightTop.dotCenters.size(), 1);
    QCOMPARE(leftBottom.dotCenters.size(), 1);
    QVERIFY(leftTop.dotCenters.constFirst().x() < rightTop.dotCenters.constFirst().x());
    QVERIFY(leftTop.dotCenters.constFirst().y() < leftBottom.dotCenters.constFirst().y());
}

void TerminalBrailleTest::extendsOneLatticeAcrossAdjacentCells() {
    const QRectF cell(0, 0, 11, 22);
    const TerminalBrailleGeometry geometry =
        TerminalBraille::geometry(QStringLiteral("⣿"), cell, 1.0);
    const TerminalBrailleGeometry right =
        TerminalBraille::geometry(QStringLiteral("⣿"), cell.translated(cell.width(), 0), 1.0);
    const TerminalBrailleGeometry below =
        TerminalBraille::geometry(QStringLiteral("⣿"), cell.translated(0, cell.height()), 1.0);

    QCOMPARE(geometry.horizontalPitch, 5.5);
    QCOMPARE(geometry.verticalPitch, 5.5);
    // Dots 1, 2, 3 and 7 run down the left column; dot 4 starts the right.
    const QVector<QPointF> &dots = geometry.dotCenters;
    QCOMPARE(dots.at(3).x() - dots.at(0).x(), geometry.horizontalPitch);
    QCOMPARE(dots.at(1).y() - dots.at(0).y(), geometry.verticalPitch);
    QCOMPARE(dots.at(2).y() - dots.at(1).y(), geometry.verticalPitch);
    QCOMPARE(dots.at(6).y() - dots.at(2).y(), geometry.verticalPitch);
    QCOMPARE(right.dotCenters.at(0).x() - geometry.dotCenters.at(3).x(),
             geometry.horizontalPitch);
    QCOMPARE(below.dotCenters.at(0).y() - geometry.dotCenters.at(6).y(),
             geometry.verticalPitch);
}

void TerminalBrailleTest::keepsDiagonalCollinearAcrossCellRows() {
    constexpr qreal cellWidth = 11.0;
    constexpr qreal cellHeight = 22.0;
    // Three consecutive source dots from logo07's x+y=29 slash. The middle
    // transition crosses from dot row 4 of one terminal row to dot row 1 of
    // the next, which caught the old per-cell vertical-margin discontinuity.
    const QPointF upper = TerminalBraille::geometry(
        QStringLiteral("⠠"), QRectF(13 * cellWidth, 0, cellWidth, cellHeight), 1.0)
                              .dotCenters.constFirst(); // logical point (27, 2)
    const QPointF boundary = TerminalBraille::geometry(
        QStringLiteral("⡀"), QRectF(13 * cellWidth, 0, cellWidth, cellHeight), 1.0)
                                 .dotCenters.constFirst(); // logical point (26, 3)
    const QPointF lower = TerminalBraille::geometry(
        QStringLiteral("⠈"), QRectF(12 * cellWidth, cellHeight, cellWidth, cellHeight), 1.0)
                              .dotCenters.constFirst(); // logical point (25, 4)

    const QPointF firstStep = boundary - upper;
    const QPointF secondStep = lower - boundary;
    QVERIFY(nearlyEqual(firstStep.x(), secondStep.x()));
    QVERIFY(nearlyEqual(firstStep.y(), secondStep.y()));
    QVERIFY(nearlyEqual(firstStep.x(), -firstStep.y()));
}

void TerminalBrailleTest::geometryTracksFontCellsAcrossDprAndSizes_data() {
    QTest::addColumn<int>("pixelSize");
    QTest::addColumn<qreal>("ratio");
    for (int pixelSize : {10, 14, 22}) {
        for (qreal ratio : {1.0, 1.25, 1.5, 2.0}) {
            const QByteArray name = QByteArray::number(pixelSize) + "px@"
                + QByteArray::number(ratio, 'f', 2);
            QTest::newRow(name.constData()) << pixelSize << ratio;
        }
    }
}

void TerminalBrailleTest::geometryTracksFontCellsAcrossDprAndSizes() {
    QFETCH(int, pixelSize);
    QFETCH(qreal, ratio);

    QFont font = TerminalFontSet::safeFixedFont(pixelSize);
    TerminalFontSet fonts;
    fonts.setFont(font);
    fonts.setDevicePixelRatio(ratio);
    const QRectF cell(0, 0, fonts.cellWidth(), fonts.cellHeight());
    const TerminalBrailleGeometry geometry =
        TerminalBraille::geometry(QStringLiteral("⣿"), cell, ratio);

    QCOMPARE(geometry.dotCenters.size(), 8);
    QVERIFY(nearlyEqual(geometry.coreDiameter * ratio, 2.0));
    QVERIFY(geometry.fringeWidth >= 0.0);
    QVERIFY(geometry.fringeWidth * ratio <= 0.5 + 0.001);
    const qreal physicalWidth = std::round(cell.right() * ratio)
        - std::round(cell.left() * ratio);
    const qreal physicalHeight = std::round(cell.bottom() * ratio)
        - std::round(cell.top() * ratio);
    QVERIFY(nearlyEqual(geometry.horizontalPitch * ratio, physicalWidth / 2.0));
    QVERIFY(nearlyEqual(geometry.verticalPitch * ratio, physicalHeight / 4.0));

    const TerminalBrailleGeometry right = TerminalBraille::geometry(
        QStringLiteral("⣿"), cell.translated(cell.width(), 0), ratio);
    const TerminalBrailleGeometry below = TerminalBraille::geometry(
        QStringLiteral("⣿"), cell.translated(0, cell.height()), ratio);
    QVERIFY(nearlyEqual(
        (right.dotCenters.at(0).x() - geometry.dotCenters.at(3).x()) * ratio,
        physicalWidth / 2.0));
    QVERIFY(nearlyEqual(
        (below.dotCenters.at(0).y() - geometry.dotCenters.at(6).y()) * ratio,
        physicalHeight / 4.0));
}

void TerminalBrailleTest::buildsClosedCoverageMesh() {
    constexpr int segments = 16;
    constexpr int verticesPerDot = 1 + segments * 2;
    constexpr int indicesPerDot = segments * 3 + segments * 6;
    const TerminalBrailleGeometry geometry =
        TerminalBraille::geometry(QStringLiteral("⣿"), QRectF(0, 0, 11, 22), 1.0);
    const TerminalCoverageMesh mesh = TerminalBrailleMeshBuilder::build(geometry);

    QVERIFY(!mesh.isEmpty());
    QCOMPARE(mesh.vertices.size(), geometry.dotCenters.size() * verticesPerDot);
    QCOMPARE(mesh.indices.size(), geometry.dotCenters.size() * indicesPerDot);
    QCOMPARE(mesh.fringeWidth, 0.5);
    QCOMPARE(dotsOffRing(mesh, geometry, segments), QList<int>());
    QVERIFY(indicesInRange(mesh));
}

void TerminalBrailleTest::mapsCommonSpinnerFrames_data()
{
    QTest::addColumn<QString>("frame");
    QTest::addColumn<int>("dots");
    QTest::newRow("U+280B") << QStringLiteral("⠋") << 3;
    QTest::newRow("U+2819") << QStringLiteral("⠙") << 3;
    QTest::newRow("U+2839") << QStringLiteral("⠹") << 4;
    QTest::newRow("U+2838") << QStringLiteral("⠸") << 3;
    QTest::newRow("U+283C") << QStringLiteral("⠼") << 4;
    QTest::newRow("U+2834") << QStringLiteral("⠴") << 3;
    QTest::newRow("U+2826") << QStringLiteral("⠦") << 3;
    QTest::newRow("U+2827") << QStringLiteral("⠧") << 4;
}

void TerminalBrailleTest::mapsCommonSpinnerFrames() {
    QFETCH(QString, frame);
    QFETCH(int, dots);
    const TerminalBrailleGeometry geometry = TerminalBraille::geometry(frame, QRectF(0, 0, 11, 22), 1.0);
    QCOMPARE(geometry.dotCenters.size(), dots);
    QVERIFY(!TerminalBrailleMeshBuilder::build(geometry).isEmpty());
}

void TerminalBrailleTest::handlesEmptyAndMinimumGeometry() {
    const TerminalBrailleGeometry empty =
        TerminalBraille::geometry(QStringLiteral("⠀"), QRectF(0, 0, 11, 22), 1.0);
    QVERIFY(empty.dotCenters.isEmpty());
    QVERIFY(TerminalBrailleMeshBuilder::build(empty).isEmpty());
    QVERIFY(TerminalBraille::geometry(
        QStringLiteral("A"), QRectF(0, 0, 11, 22), 1.0).isEmpty());

    const QRectF minimumCell(0, 0, 4, 8);
    const TerminalBrailleGeometry minimum =
        TerminalBraille::geometry(QStringLiteral("⣿"), minimumCell, 1.0);
    QCOMPARE(minimum.dotCenters.size(), 8);
    QCOMPARE(minimum.coreDiameter, 2.0);
    QCOMPARE(minimum.fringeWidth, 0.0);
    const TerminalCoverageMesh mesh = TerminalBrailleMeshBuilder::build(minimum);
    QVERIFY(!mesh.isEmpty());
    QCOMPARE(coresOutside(minimum.dotCenters, minimumCell), QVector<QPointF>());
}

QTEST_MAIN(TerminalBrailleTest)

#include "terminal_braille_test.moc"
