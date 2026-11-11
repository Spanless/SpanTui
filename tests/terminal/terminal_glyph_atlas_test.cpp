#include "terminal/render/terminal_cell_material.h"
#include "terminal/render/terminal_drawn_glyphs.h"
#include "terminal/render/terminal_glyph_atlas.h"
#include "terminal/render/terminal_row_geometry.h"
#include "terminal/render/terminal_row_layout.h"

#include <QTest>

#include <algorithm>
#include <cmath>

using namespace span::terminal;

namespace {

TerminalFontSet fontSet(int pixelSize, qreal devicePixelRatio = 1.0)
{
    TerminalFontSet fonts;
    QFont font(QStringLiteral("Consolas"));
    font.setPixelSize(pixelSize);
    fonts.setFont(font);
    fonts.setDevicePixelRatio(devicePixelRatio);
    return fonts;
}

Line textLine(const QString &text)
{
    Line line;
    for (const QChar character : text) {
        Cell cell;
        cell.text = QString(character);
        cell.width = 1;
        line.cells.push_back(cell);
    }
    return line;
}

// `text` with explicit colours; an invalid colour keeps the default.
Line coloredLine(const QString &text, const QColor &foreground, const QColor &background = QColor())
{
    Line line = textLine(text);
    for (Cell &cell : line.cells) {
        cell.style.defaultForeground = !foreground.isValid();
        cell.style.foreground = foreground;
        cell.style.defaultBackground = !background.isValid();
        cell.style.background = background;
    }
    return line;
}

// Every BMP character from `first` to `last`.
QString characterRange(char16_t first, char16_t last)
{
    QString text;
    for (char16_t character = first; character <= last; ++character) text += QChar(character);
    return text;
}

// The 64 braille cells with dots 1 and 8 (opposite corners) set.
QString brailleWithCornerDots()
{
    QString text;
    for (int dots = 0; dots < 64; ++dots) text += QChar(0x2800 | 0x81 | (dots << 1));
    return text;
}

// Full coverage for the mask page.
QImage coverage(int width, int height)
{
    QImage image(width, height, QImage::Format_Alpha8);
    image.fill(255);
    return image;
}

// How many copies of `image` fit before the page is full.
int insertUntilFull(TerminalGlyphAtlas &atlas, TerminalGlyphAtlas::Page page, const QImage &image)
{
    int placed = 0;
    while (atlas.insert(page, image)) ++placed;
    return placed;
}

// How many of `count` copies of `image` the page took.
int insertCopies(TerminalGlyphAtlas &atlas, TerminalGlyphAtlas::Page page, const QImage &image, int count)
{
    int placed = 0;
    for (int index = 0; index < count; ++index) placed += atlas.insert(page, image).has_value();
    return placed;
}

// Cells whose glyph loaded in bulk differs from the same glyph rasterized
// alone, or has no texels.
QStringList bulkLoadMismatches(TerminalGlyphCache &loaded, TerminalGlyphCache &single, const TerminalRowLayout &layout)
{
    QStringList mismatches;
    for (const TerminalGlyphCell &cell : layout.glyphCells) {
        const TerminalGlyph fromLoad = loaded.glyph(cell);
        const TerminalGlyph alone = single.glyph(cell);
        if (fromLoad.quad != alone.quad || fromLoad.texels.size() != alone.texels.size()
            || fromLoad.field != alone.field || fromLoad.texels.isNull())
            mismatches.append(cell.text);
    }
    return mismatches;
}

// Cells whose lowest opaque pixel is more than a device pixel off `baseline`.
QStringList inkOffBaseline(const QImage &image, const QRectF &bounds, qreal dpr, const TerminalRowLayout &layout,
                           qreal baseline)
{
    QStringList off;
    for (const TerminalGlyphCell &cell : layout.glyphCells) {
        const int left = std::max(0, int(std::lround((cell.cell.left() - bounds.left()) * dpr)));
        const int right = std::min(image.width(), int(std::lround((cell.cell.right() - bounds.left()) * dpr)));
        int bottom = -1;
        for (int y = 0; y < image.height(); ++y) {
            for (int x = left; x < right; ++x) {
                if (qAlpha(image.pixel(x, y)) >= 128) bottom = y;
            }
        }
        const qreal inkBottom = bounds.top() + (bottom + 1) / dpr;
        if (bottom < 0 || std::abs(inkBottom - baseline) > 1.0 / dpr + 0.001)
            off.append(QStringLiteral("%1: %2 vs %3").arg(cell.text).arg(inkBottom).arg(baseline));
    }
    return off;
}

// Asks for chrome glyphs in order until the cache moves to a new atlas;
// the number asked, or -1 if one came back without texels.
int chromeUntilAtlasMoves(TerminalGlyphCache &cache, const QVector<TerminalCustomGlyph> &glyphs)
{
    const std::shared_ptr<const TerminalGlyphAtlas> first = cache.atlas();
    int placed = 0;
    while (placed < glyphs.size() && cache.atlas() == first) {
        if (cache.chrome(glyphs.at(placed++)).texels.isNull()) return -1;
    }
    return placed;
}

void chromeFirst(TerminalGlyphCache &cache, const QVector<TerminalCustomGlyph> &glyphs, int count)
{
    for (int index = 0; index < count; ++index) cache.chrome(glyphs.at(index));
}

template <typename Field> auto vertexField(const TerminalRowGeometry &geometry, Field field)
{
    QList<decltype(field(geometry.vertices.first()))> values;
    for (const TerminalCellVertex &vertex : geometry.vertices) values.append(field(vertex));
    return values;
}

// Coverage at `steps + 1` evenly spaced distances from 0 to 1.
QList<float> coverageSteps(float scale, int steps)
{
    QList<float> values;
    for (int step = 0; step <= steps; ++step) values.append(TerminalFieldText::coverage(float(step) / steps, scale));
    return values;
}

struct InkCounts {
    int solid = 0;
    int blended = 0;
    int foreign = 0;
};

// Pixels of the first `width` columns: exactly `ink`, an opaque blend of
// green over black, or carrying red or blue.
InkCounts greenInk(const QImage &image, int width)
{
    InkCounts counts;
    for (int y = 0; y < image.height(); ++y) {
        for (int x = 0; x < width; ++x) {
            const QColor color = image.pixelColor(x, y);
            counts.foreign += color.red() != 0 || color.blue() != 0;
            counts.solid += color == QColor(0, 255, 0);
            counts.blended += color.alpha() == 255 && color.green() > 0 && color.green() < 255;
        }
    }
    return counts;
}

constexpr auto kMask = TerminalGlyphAtlas::Page::Mask;
constexpr auto kColor = TerminalGlyphAtlas::Page::Color;

} // namespace

class TerminalGlyphAtlasTest final : public QObject {
    Q_OBJECT

private slots:
    void uploadsOnlyWhatIsNew();
    void reportsAFullAtlas();
    void rasterizesEachGlyphOnce();
    void loadsNewGlyphsOnHelperThreads();
    void drawsStyledGlyphsOnOneBaseline();
    void movesRecentGlyphsIntoALargerAtlas();
    void keepsRowIndicesForColourOnlyChanges();
    void marksFieldGlyphsForTheShader();
    void turnsFieldsIntoCoverageLikeQtQuick();
    void fieldCoverageRisesFromZeroToOne_data();
    void fieldCoverageRisesFromZeroToOne();
    void paintsRowsOnTheCpuLikeTheShader();
};

void TerminalGlyphAtlasTest::uploadsOnlyWhatIsNew()
{
    TerminalGlyphAtlas atlas(64, 16);
    // A new texture has nothing yet: it gets the rows used so far, the
    // opaque block and its transparent border.
    quint64 revision = 0;
    QVector<TerminalGlyphAtlas::Upload> uploads = atlas.takeUploads(kMask, revision);
    QCOMPARE(uploads.size(), 1);
    QCOMPARE(uploads.first().image.size(), QSize(64, 4));
    QCOMPARE(uploads.first().image.constScanLine(1)[1], uchar(255));
    QCOMPARE(uploads.first().image.constScanLine(3)[3], uchar(0));
    QVERIFY(atlas.takeUploads(kMask, revision).isEmpty());

    // Each placement goes up with the transparent texels right of and below it.
    const std::optional<QPoint> first = atlas.insert(kMask, coverage(5, 3));
    const std::optional<QPoint> second = atlas.insert(kMask, coverage(4, 6));
    QVERIFY(first && second);
    QVERIFY(!QRect(*first, QSize(5, 3)).intersects(QRect(*second, QSize(4, 6))));
    uploads = atlas.takeUploads(kMask, revision);
    QCOMPARE(uploads.size(), 2);
    QCOMPARE(uploads.at(0).position, *first);
    QCOMPARE(uploads.at(1).image.size(), QSize(5, 7));
    QCOMPARE(uploads.at(1).image.constScanLine(5)[3], uchar(255));
    QCOMPARE(uploads.at(1).image.constScanLine(6)[4], uchar(0));
    QVERIFY(atlas.takeUploads(kMask, revision).isEmpty());

    // The colour page starts empty and keeps its own revisions.
    quint64 colorRevision = 0;
    QVERIFY(atlas.takeUploads(kColor, colorRevision).isEmpty());
    QImage emoji(3, 3, QImage::Format_RGBA8888_Premultiplied);
    emoji.fill(QColor(255, 0, 0));
    QVERIFY(atlas.insert(kColor, emoji));
    uploads = atlas.takeUploads(kColor, colorRevision);
    QCOMPARE(uploads.size(), 1);
    QCOMPARE(uploads.first().image.pixelColor(0, 0), QColor(255, 0, 0));
    QVERIFY(atlas.takeUploads(kMask, revision).isEmpty());
}

void TerminalGlyphAtlasTest::reportsAFullAtlas()
{
    TerminalGlyphAtlas atlas(32, 16);
    QVERIFY(!atlas.insert(kMask, coverage(40, 4)));
    // Four per shelf (7 + 1 padding), three beside the opaque texel.
    QCOMPARE(insertUntilFull(atlas, kMask, coverage(7, 7)), 15);
    QCOMPARE(atlas.usage(kMask), 1.0);

    // A texture further behind than the change log reaches reloads the rows
    // used: one shelf of 510 beside the opaque block, then shelves of 512,
    // two texels high each.
    TerminalGlyphAtlas large(1024, 16);
    quint64 revision = 1;
    QCOMPARE(insertCopies(large, kMask, coverage(1, 1), 5000), 5000);
    const QVector<TerminalGlyphAtlas::Upload> uploads = large.takeUploads(kMask, revision);
    QCOMPARE(uploads.size(), 1);
    QCOMPARE(uploads.first().image.size(), QSize(1024, 22));
}

void TerminalGlyphAtlasTest::rasterizesEachGlyphOnce()
{
    const TerminalFontSet fonts = fontSet(14);
    TerminalGlyphCache cache;
    cache.setFonts(fonts);
    const TerminalRowLayout layout =
        TerminalRowLayoutBuilder::build(textLine(QStringLiteral("M M")), fonts, Qt::black, Qt::white, {});
    QCOMPARE(layout.glyphCells.size(), 3);
    const TerminalGlyph left = cache.glyph(layout.glyphCells.at(0));
    const TerminalGlyph space = cache.glyph(layout.glyphCells.at(1));
    const TerminalGlyph right = cache.glyph(layout.glyphCells.at(2));

    QVERIFY(!left.texels.isNull());
    QCOMPARE(right.texels, left.texels);
    QCOMPARE(right.quad, left.quad);
    QVERIFY(left.field);
    QVERIFY(!left.color);
    // The field, margin included, stays near its cell; a blank cell draws
    // nothing.
    QVERIFY(QRectF(-2, -2, fonts.cellWidth() + 4, fonts.cellHeight() + 4).contains(left.quad));
    QVERIFY(space.quad.isEmpty());
    QVERIFY(space.texels.isNull());

    // "M" is in the atlas once, after the opaque texel.
    quint64 revision = 1;
    QCOMPARE(cache.atlas()->takeUploads(kMask, revision).size(), 1);

    // New metrics start a new atlas.
    const std::shared_ptr<const TerminalGlyphAtlas> before = cache.atlas();
    cache.setFonts(fonts);
    QCOMPARE(cache.atlas(), before);
    cache.setFonts(fontSet(14, 2.0));
    QVERIFY(cache.atlas() != before);
    QCOMPARE(cache.atlas()->size(kMask), 2048);
    QCOMPARE(cache.atlas()->size(kColor), 512);
}

void TerminalGlyphAtlasTest::loadsNewGlyphsOnHelperThreads()
{
    // Enough distinct text for helper threads.
    const QString text = characterRange(u'!', u'~') + characterRange(0x0410, 0x044F);
    const TerminalFontSet fonts = fontSet(14);
    const TerminalRowLayout layout = TerminalRowLayoutBuilder::build(textLine(text), fonts, Qt::black, Qt::white, {});
    QCOMPARE(layout.glyphCells.size(), text.size());

    TerminalGlyphCache loaded;
    loaded.setFonts(fonts);
    loaded.load({&layout});
    quint64 revision = 0;
    loaded.atlas()->takeUploads(kMask, revision);

    // Every glyph is in the atlas already, as one at a time would have
    // rasterized it.
    TerminalGlyphCache single;
    single.setFonts(fonts);
    QCOMPARE(bulkLoadMismatches(loaded, single, layout), QStringList());
    QVERIFY(loaded.atlas()->takeUploads(kMask, revision).isEmpty());
}

void TerminalGlyphAtlasTest::drawsStyledGlyphsOnOneBaseline()
{
    const qreal dpr = 1.5;
    const TerminalFontSet fonts = fontSet(14, dpr);
    TerminalGlyphCache cache;
    cache.setFonts(fonts);
    Line line = textLine(QStringLiteral("NBI"));
    line.cells[1].style.bold = true;
    line.cells[2].style.italic = true;
    const TerminalRowLayout layout = TerminalRowLayoutBuilder::build(line, fonts, Qt::black, Qt::white, {});
    const TerminalRowGeometry geometry = TerminalRowGeometryBuilder::build(layout, cache, dpr);
    TerminalCellRasterizer rasterizer(geometry.atlas);
    QRectF bounds;
    const QImage image = rasterizer.paint(geometry.vertices, geometry.indices, dpr, &bounds);
    QVERIFY(!image.isNull());

    // Capitals without descenders end on the baseline, which sits on a
    // device pixel whatever the style.
    const qreal baseline = TerminalGlyphGeometry::snap(fonts.baseline(), dpr);
    QCOMPARE(inkOffBaseline(image, bounds, dpr, layout, baseline), QStringList());
}

void TerminalGlyphAtlasTest::movesRecentGlyphsIntoALargerAtlas()
{
    // Text fields do not grow with the font; braille cells this large, each
    // with dots in two opposite corners, fill the first mask page within a
    // few dozen.
    const TerminalFontSet fonts = fontSet(700);
    TerminalGlyphCache cache;
    cache.setFonts(fonts);
    const std::shared_ptr<const TerminalGlyphAtlas> first = cache.atlas();
    QCOMPARE(first->size(kMask), 2048);
    const QString text = brailleWithCornerDots();
    const TerminalRowLayout layout = TerminalRowLayoutBuilder::build(textLine(text), fonts, Qt::black, Qt::white, {});
    QCOMPARE(layout.customGlyphs.size(), 64);
    const int placed = chromeUntilAtlasMoves(cache, layout.customGlyphs);
    QVERIFY(placed > 0);
    QVERIFY(cache.atlas() != first);
    QCOMPARE(cache.atlas()->size(kMask), 4096);
    QCOMPARE(cache.atlas()->size(kColor), 512);

    // The glyphs placed before moved over texel for texel: asking for them
    // again rasterizes nothing.
    quint64 revision = 0;
    cache.atlas()->takeUploads(kMask, revision);
    chromeFirst(cache, layout.customGlyphs, placed);
    QVERIFY(cache.atlas()->takeUploads(kMask, revision).isEmpty());

    // A row of them samples the new atlas only.
    const TerminalRowGeometry geometry = TerminalRowGeometryBuilder::build(
        TerminalRowLayoutBuilder::build(textLine(text.left(placed)), fonts, Qt::black, Qt::white, {}), cache, 1.0);
    QCOMPARE(geometry.atlas, cache.atlas());
    QCOMPARE(geometry.vertices.size(), placed * 4);
}

void TerminalGlyphAtlasTest::keepsRowIndicesForColourOnlyChanges()
{
    const TerminalFontSet fonts = fontSet(14);
    TerminalGlyphCache cache;
    cache.setFonts(fonts);
    const QString text = QStringLiteral("a⣿");
    Line second = coloredLine(text, QColor(108, 108, 108));
    second.cells[0].style.foreground = QColor(200, 100, 50);
    second.cells[1].style.foreground = QColor(180, 180, 180);

    const TerminalRowGeometry before = TerminalRowGeometryBuilder::build(
        TerminalRowLayoutBuilder::build(coloredLine(text, QColor(108, 108, 108)), fonts, Qt::white, Qt::black, {}),
        cache, 1.0);
    const TerminalRowGeometry after = TerminalRowGeometryBuilder::build(
        TerminalRowLayoutBuilder::build(second, fonts, Qt::white, Qt::black, {}), cache, 1.0);
    const auto x = [](const TerminalCellVertex &vertex) { return vertex.x; };
    const auto u = [](const TerminalCellVertex &vertex) { return vertex.u; };
    const auto red = [](const TerminalCellVertex &vertex) { return int(vertex.r); };
    QCOMPARE(after.indices, before.indices);
    QCOMPARE(vertexField(after, x), vertexField(before, x));
    QCOMPARE(vertexField(after, u), vertexField(before, u));
    QVERIFY(vertexField(after, red) != vertexField(before, red));
}

void TerminalGlyphAtlasTest::marksFieldGlyphsForTheShader()
{
    const TerminalFontSet fonts = fontSet(14);
    TerminalGlyphCache cache;
    cache.setFonts(fonts);
    const TerminalRowGeometry geometry = TerminalRowGeometryBuilder::build(
        TerminalRowLayoutBuilder::build(coloredLine(QStringLiteral("a⣿"), QColor(), QColor(30, 60, 90)), fonts,
                                        Qt::white, Qt::black, {}),
        cache, 1.0);

    // Only the text glyph's quad samples a distance field; the background
    // and the braille cell are colour times mask texel.
    const QList<int> fields = vertexField(geometry, [](const TerminalCellVertex &vertex) { return int(vertex.field); });
    const QList<int> colors = vertexField(geometry, [](const TerminalCellVertex &vertex) { return int(vertex.color); });
    QCOMPARE(fields.count(255), 4);
    QCOMPARE(fields.count(0) + 4, fields.size());
    QVERIFY(fields.count(0) >= 8);
    QCOMPARE(colors.count(0), colors.size());
}

void TerminalGlyphAtlasTest::turnsFieldsIntoCoverageLikeQtQuick()
{
    // Large enough text is cut on the outline, sharper the larger it is.
    const auto [low, high] = TerminalFieldText::alphaRange(0.5f);
    QVERIFY(std::abs(low - 0.38f) < 0.0001f);
    QVERIFY(std::abs(high - 0.62f) < 0.0001f);
    QVERIFY(std::abs(TerminalFieldText::coverage(0.5f, 0.5f) - 0.5f) < 0.0001f);
    // Small text is cut a little outside the outline, which makes it bolder.
    const auto [smallLow, smallHigh] = TerminalFieldText::alphaRange(0.15f);
    QVERIFY(std::abs((smallLow + smallHigh) / 2 - 0.435f) < 0.0001f);
    QVERIFY(TerminalFieldText::coverage(0.5f, 0.15f) > 0.5f);
}

void TerminalGlyphAtlasTest::fieldCoverageRisesFromZeroToOne_data()
{
    QTest::addColumn<float>("scale");
    QTest::newRow("small") << 0.15f;
    QTest::newRow("medium") << 0.42f;
    QTest::newRow("large") << 1.0f;
}

void TerminalGlyphAtlasTest::fieldCoverageRisesFromZeroToOne()
{
    QFETCH(float, scale);
    const QList<float> steps = coverageSteps(scale, 16);
    QCOMPARE(steps.first(), 0.0f);
    QCOMPARE(steps.last(), 1.0f);
    QVERIFY(std::is_sorted(steps.begin(), steps.end()));
}

void TerminalGlyphAtlasTest::paintsRowsOnTheCpuLikeTheShader()
{
    const qreal dpr = 2.0;
    const TerminalFontSet fonts = fontSet(14, dpr);
    TerminalGlyphCache cache;
    cache.setFonts(fonts);
    // Contrasted enough that contrast repair leaves the ink as is.
    const TerminalRowGeometry geometry = TerminalRowGeometryBuilder::build(
        TerminalRowLayoutBuilder::build(coloredLine(QStringLiteral("M "), QColor(0, 255, 0), QColor(0, 0, 0)), fonts,
                                        Qt::white, Qt::gray, {}),
        cache, dpr);

    TerminalCellRasterizer rasterizer(geometry.atlas);
    QRectF bounds;
    const QImage image = rasterizer.paint(geometry.vertices, geometry.indices, dpr, &bounds);
    QVERIFY(!image.isNull());
    QCOMPARE(image.size(), (bounds.size() * dpr).toSize());
    QVERIFY(bounds.contains(QRectF(0, 0, 2 * fonts.cellWidth(), fonts.cellHeight())));

    // The blank cell is its background alone; the glyph's stems are solid
    // foreground over it, its edges a blend of both.
    const QPoint blank =
        ((QPointF(1.5 * fonts.cellWidth(), 0.5 * fonts.cellHeight()) - bounds.topLeft()) * dpr).toPoint();
    QCOMPARE(image.pixelColor(blank), QColor(0, 0, 0));
    const InkCounts ink = greenInk(image, int(fonts.cellWidth() * dpr));
    QCOMPARE(ink.foreign, 0);
    QVERIFY(ink.solid > 0);
    QVERIFY(ink.blended > 0);
}

QTEST_MAIN(TerminalGlyphAtlasTest)

#include "terminal_glyph_atlas_test.moc"
