#include "terminal/core/terminal_color.h"
#include "terminal/render/terminal_row_layout.h"

#include <QTest>

#include <algorithm>

using namespace span::terminal;

namespace {
Cell cell(const QString &text, int width = 1) {
    Cell result;
    result.text = text;
    result.width = width;
    return result;
}

// Applies `edit` to the style of each cell in columns [from, to).
template <typename Edit> void styleColumns(Line &line, int from, int to, Edit edit)
{
    for (int column = from; column < to; ++column) edit(line.cells[column].style);
}

QString glyphPlacement(int firstColumn, int columns, qreal x, qreal letterSpacing)
{
    return QStringLiteral("%1+%2 at %3 spacing %4").arg(firstColumn).arg(columns).arg(x).arg(letterSpacing);
}

QStringList glyphPlacements(const TerminalRowLayout &layout)
{
    QStringList placements;
    for (const TerminalGlyphCell &glyph : layout.glyphCells)
        placements.append(glyphPlacement(glyph.firstColumn, glyph.columns, glyph.cell.x(), glyph.font.letterSpacing()));
    return placements;
}

bool paintsBackground(const TerminalRowLayout &layout, const QColor &color)
{
    return std::any_of(layout.backgroundRuns.cbegin(), layout.backgroundRuns.cend(),
                       [&](const TerminalBackgroundRun &run) { return run.color == color; });
}
}

class TerminalRowLayoutTest final : public QObject {
    Q_OBJECT

private slots:
    void emitsOneGlyphCellPerVisibleCell();
    void keepsWideCellsOnTheirTwoCellSpan();
    void resolvesAnsiBackgroundsAndSnapsToDpr();
    void resolvesOneSemanticFrameAgainstEachTheme();
    void omitsDefaultBackgroundForTransparentVisualPlane();
    void rendersDefaultColorReverseVideoBlankCell();
    void concealedCellsPaintOnlyTheirBackground();
    void preservesAFullBlankCellAcrossStyleBoundary();
    void sharesOneFontPerStyle();
    void keepsCombinedCharactersInOneCell();
    void replacesCodePointsNoFontMaps();
    void keepsMathematicalOperatorsOnTheFontGlyphPath();
    void raisesTruecolorForegroundToReadableContrast();
    void leavesReadableThemeForegroundUnchanged();
    void leavesDimBlockChromeUnchanged();
    void raisesDefaultInkOnDarkComposerBar();
    void raisesAnsiDimOnDarkComposerBar();
    void fadesFaintInkTowardTheBackground();
    void resolvesUnderlineColorAndOverline();
};

void TerminalRowLayoutTest::emitsOneGlyphCellPerVisibleCell() {
    TerminalFontSet fonts;
    QFont font(QStringLiteral("Consolas"));
    font.setPixelSize(14);
    fonts.setFont(font);

    Line line;
    line.cells = {cell(QStringLiteral("a")), cell(QStringLiteral("b")), cell(QStringLiteral("c"))};
    const TerminalRowLayout layout = TerminalRowLayoutBuilder::build(
        line, fonts, Qt::black, Qt::white, {});

    QCOMPARE(glyphPlacements(layout),
             (QStringList{glyphPlacement(0, 1, 0, 0), glyphPlacement(1, 1, fonts.cellWidth(), 0),
                          glyphPlacement(2, 1, fonts.cellWidth() * 2, 0)}));
}

void TerminalRowLayoutTest::keepsWideCellsOnTheirTwoCellSpan() {
    TerminalFontSet fonts;
    QFont font(QStringLiteral("Consolas"));
    font.setPixelSize(14);
    fonts.setFont(font);

    Line line;
    line.cells = {cell(QStringLiteral("A")), cell(QStringLiteral("中"), 2), Cell{}, cell(QStringLiteral("B"))};
    line.cells[2].continuation = true;
    const TerminalRowLayout layout = TerminalRowLayoutBuilder::build(
        line, fonts, Qt::black, Qt::white, {});

    QCOMPARE(layout.glyphCells.size(), 3);
    QCOMPARE(layout.glyphCells.at(1).firstColumn, 1);
    QCOMPARE(layout.glyphCells.at(1).columns, 2);
    QCOMPARE(layout.glyphCells.at(1).cell.width(), fonts.cellWidth() * 2.0);
    QCOMPARE(layout.glyphCells.at(2).firstColumn, 3);
    QCOMPARE(layout.glyphCells.at(2).cell.x(), fonts.cellWidth() * 3.0);
}

void TerminalRowLayoutTest::resolvesAnsiBackgroundsAndSnapsToDpr() {
    TerminalFontSet fonts;
    QFont font(QStringLiteral("Consolas"));
    font.setPixelSize(14);
    fonts.setFont(font);
    fonts.setDevicePixelRatio(1.5);

    Line line;
    line.cells = {cell(QStringLiteral("x")), cell(QStringLiteral("y"))};
    line.cells[0].style.defaultBackground = false;
    line.cells[0].style.backgroundPaletteIndex = 1;
    line.cells[1].style.defaultBackground = false;
    line.cells[1].style.backgroundPaletteIndex = 1;
    const QColor red(QStringLiteral("#b4233c"));
    const TerminalRowLayout layout = TerminalRowLayoutBuilder::build(
        line, fonts, Qt::black, Qt::white, {Qt::black, red});

    QCOMPARE(layout.backgroundRuns.size(), 1);
    QCOMPARE(layout.backgroundRuns.at(0).color, red);
    const qreal physicalX = layout.backgroundRuns.at(0).rect.right() * fonts.devicePixelRatio();
    QVERIFY(qAbs(physicalX - std::round(physicalX)) < 0.001);
}

void TerminalRowLayoutTest::concealedCellsPaintOnlyTheirBackground() {
    TerminalFontSet fonts;
    QFont font(QStringLiteral("Consolas"));
    font.setPixelSize(14);
    fonts.setFont(font);

    // Default ink, palette ink on a palette background, and an underlined
    // box-drawing glyph: each path that can re-ink concealed text.
    Line line;
    line.cells = {cell(QStringLiteral("a")), cell(QStringLiteral("b")),
                  cell(QStringLiteral("─")), cell(QStringLiteral("v"))};
    styleColumns(line, 0, 3, [](CellStyle &style) { style.conceal = true; });
    line.cells[1].style.defaultForeground = false;
    line.cells[1].style.foregroundPaletteIndex = 1;
    line.cells[1].style.defaultBackground = false;
    line.cells[1].style.backgroundPaletteIndex = 4;
    line.cells[2].style.underline = UnderlineStyle::Single;
    const QVector<QColor> palette = defaultAnsiPalette(true);
    const TerminalRowLayout layout = TerminalRowLayoutBuilder::build(
        line, fonts, defaultSurfaceForeground(true), defaultSurfaceBackground(true), palette);

    QCOMPARE(layout.glyphCells.size(), 1);
    QCOMPARE(layout.glyphCells.at(0).firstColumn, 3);
    QVERIFY(layout.customGlyphs.isEmpty());
    QVERIFY(layout.decorations.isEmpty());
    QVERIFY(paintsBackground(layout, palette.at(4)));
}

void TerminalRowLayoutTest::resolvesOneSemanticFrameAgainstEachTheme() {
    TerminalFontSet fonts;
    QFont font(QStringLiteral("Consolas"));
    font.setPixelSize(14);
    fonts.setFont(font);

    Line line;
    line.cells = {cell(QStringLiteral("D")), cell(QStringLiteral("A"))};
    line.cells[1].style.defaultForeground = false;
    line.cells[1].style.foregroundPaletteIndex = 4;

    const TerminalRowLayout light = TerminalRowLayoutBuilder::build(
        line, fonts, defaultSurfaceForeground(false),
        defaultSurfaceBackground(false), defaultAnsiPalette(false));
    const TerminalRowLayout dark = TerminalRowLayoutBuilder::build(
        line, fonts, defaultSurfaceForeground(true),
        defaultSurfaceBackground(true), defaultAnsiPalette(true));

    QCOMPARE(light.glyphCells.size(), 2);
    QCOMPARE(dark.glyphCells.size(), 2);
    QCOMPARE(light.glyphCells.at(0).color, defaultSurfaceForeground(false));
    QCOMPARE(dark.glyphCells.at(0).color, defaultSurfaceForeground(true));
    QCOMPARE(light.glyphCells.at(1).color, defaultAnsiPalette(false).at(4));
    QCOMPARE(dark.glyphCells.at(1).color, defaultAnsiPalette(true).at(4));
    QVERIFY(light.glyphCells.at(0).color != dark.glyphCells.at(0).color);
    QVERIFY(light.glyphCells.at(1).color != dark.glyphCells.at(1).color);
}

void TerminalRowLayoutTest::omitsDefaultBackgroundForTransparentVisualPlane() {
    TerminalFontSet fonts;
    QFont font(QStringLiteral("Consolas"));
    font.setPixelSize(14);
    fonts.setFont(font);

    Line line;
    line.cells = {cell(QStringLiteral("a")), cell(QStringLiteral(" ")),
                  cell(QStringLiteral("b"))};
    const QColor semanticBackground(QStringLiteral("#090b13"));
    const TerminalRowLayout layout = TerminalRowLayoutBuilder::build(
        line, fonts, Qt::white, semanticBackground, {});

    // The opaque default remains available for color resolution, reverse
    // video and IME, but it must not paint over the QML glass surface.
    QVERIFY(layout.backgroundRuns.isEmpty());
}

void TerminalRowLayoutTest::rendersDefaultColorReverseVideoBlankCell() {
    TerminalFontSet fonts;
    QFont font(QStringLiteral("Consolas"));
    font.setPixelSize(14);
    fonts.setFont(font);

    Line line;
    line.cells = {cell(QStringLiteral(" ")), cell(QStringLiteral(" "))};
    line.cells[0].style.reverse = true;
    const QColor foreground(QStringLiteral("#e6e6e6"));
    const QColor background(QStringLiteral("#101010"));
    const TerminalRowLayout layout = TerminalRowLayoutBuilder::build(
        line, fonts, foreground, background, {});

    QCOMPARE(layout.backgroundRuns.size(), 1);
    QCOMPARE(layout.backgroundRuns.at(0).color, foreground);
    QCOMPARE(layout.backgroundRuns.at(0).rect.x(), 0.0);
    QCOMPARE(layout.backgroundRuns.at(0).rect.width(), fonts.cellWidth());
}

void TerminalRowLayoutTest::preservesAFullBlankCellAcrossStyleBoundary() {
    TerminalFontSet fonts;
    QFont font(QStringLiteral("Consolas"));
    font.setPixelSize(14);
    fonts.setFont(font);
    fonts.setDevicePixelRatio(1.5);

    Line line;
    line.cells = {cell(QString::fromUtf8(u8"●")), cell(QStringLiteral(" ")),
                  cell(QStringLiteral("T")), cell(QStringLiteral("i")), cell(QStringLiteral("p")),
                  cell(QStringLiteral(" ")), cell(QStringLiteral("U")), cell(QStringLiteral("s"))};
    styleColumns(line, 2, 6, [](CellStyle &style) {
        style.bold = true;
        style.defaultForeground = false;
        style.foregroundPaletteIndex = 3;
    });
    const TerminalRowLayout layout = TerminalRowLayoutBuilder::build(
        line, fonts, Qt::black, Qt::white, {Qt::black, Qt::black, Qt::black, Qt::darkYellow});

    QCOMPARE(layout.glyphCells.size(), 8);
    const TerminalGlyphCell &tipLast = layout.glyphCells.at(4);
    const TerminalGlyphCell &gap = layout.glyphCells.at(5);
    const TerminalGlyphCell &useFirst = layout.glyphCells.at(6);
    QCOMPARE(tipLast.text, QStringLiteral("p"));
    QCOMPARE(gap.text, QStringLiteral(" "));
    QCOMPARE(useFirst.text, QStringLiteral("U"));
    QCOMPARE(useFirst.cell.x() - tipLast.cell.x(), fonts.cellWidth() * 2.0);
    QCOMPARE(useFirst.cell.x() - gap.cell.x(), fonts.cellWidth());
    QVERIFY(tipLast.font.bold());
    QVERIFY(!useFirst.font.bold());
    QVERIFY(contrastRatio(tipLast.color, Qt::white) >= kMinimumReadableContrast);
    QCOMPARE(useFirst.color, Qt::black);
}

void TerminalRowLayoutTest::sharesOneFontPerStyle()
{
    TerminalFontSet fonts;
    QFont font(QStringLiteral("Consolas"));
    font.setPixelSize(14);
    fonts.setFont(font);

    Line line;
    line.cells = {cell(QStringLiteral("a")), cell(QStringLiteral("B")), cell(QStringLiteral("c")),
                  cell(QStringLiteral("D"))};
    line.cells[1].style.bold = true;
    line.cells[3].style.bold = true;
    const TerminalRowLayout layout = TerminalRowLayoutBuilder::build(line, fonts, Qt::black, Qt::white, {});

    // The glyph cache tells fonts apart by their shared data first; cells of
    // one style must not each carry a separately built font.
    QCOMPARE(layout.glyphCells.size(), 4);
    QVERIFY(layout.glyphCells.at(0).font.isCopyOf(layout.glyphCells.at(2).font));
    QVERIFY(layout.glyphCells.at(1).font.isCopyOf(layout.glyphCells.at(3).font));
    QVERIFY(!layout.glyphCells.at(0).font.isCopyOf(layout.glyphCells.at(1).font));
}

void TerminalRowLayoutTest::keepsCombinedCharactersInOneCell() {
    TerminalFontSet fonts;
    QFont font(QStringLiteral("Consolas"));
    font.setPixelSize(14);
    fonts.setFont(font);
    fonts.setDevicePixelRatio(1.25);

    Line line;
    line.cells = {cell(QString::fromUtf8(u8"e\u0301")), cell(QStringLiteral("X"))};
    const TerminalRowLayout layout = TerminalRowLayoutBuilder::build(
        line, fonts, Qt::black, Qt::white, {});

    QCOMPARE(layout.glyphCells.size(), 2);
    QCOMPARE(layout.glyphCells.at(0).text, QString::fromUtf8(u8"e\u0301"));
    QCOMPARE(layout.glyphCells.at(0).columns, 1);
    QCOMPARE(layout.glyphCells.at(1).cell.x(), fonts.cellWidth());
    const qreal physicalX = layout.glyphCells.at(1).cell.x() * fonts.devicePixelRatio();
    QVERIFY(qAbs(physicalX - std::round(physicalX)) < 0.001);
}

void TerminalRowLayoutTest::replacesCodePointsNoFontMaps()
{
    TerminalFontSet fonts;
    QFont font(QStringLiteral("Consolas"));
    font.setPixelSize(14);
    fonts.setFont(font);

    // U+0378 is unassigned, so no font maps it.
    const QString unmapped(QChar(0x0378));
    Line line;
    line.cells = {cell(unmapped), cell(QString::fromUtf8(u8"éé")), cell(QStringLiteral("a") + unmapped)};
    const TerminalRowLayout layout = TerminalRowLayoutBuilder::build(line, fonts, Qt::black, Qt::white, {});

    QCOMPARE(layout.glyphCells.size(), 3);
    QCOMPARE(layout.glyphCells.at(0).text, QString(QChar(0xFFFD)));
    QCOMPARE(layout.glyphCells.at(1).text, QString::fromUtf8(u8"éé"));
    QCOMPARE(layout.glyphCells.at(2).text, QStringLiteral("a") + QChar(0xFFFD));
    QCOMPARE(line.cells.at(0).text, unmapped);
}

void TerminalRowLayoutTest::keepsMathematicalOperatorsOnTheFontGlyphPath() {
    TerminalFontSet fonts;
    QFont font(QStringLiteral("Consolas"));
    font.setPixelSize(14);
    fonts.setFont(font);

    Line line;
    line.cells = {cell(QString::fromUtf8(u8"≡"))};
    line.cells[0].style.bold = true;
    const TerminalRowLayout layout = TerminalRowLayoutBuilder::build(
        line, fonts, Qt::black, Qt::white, {});

    QCOMPARE(layout.customGlyphs.size(), 0);
    QCOMPARE(layout.glyphCells.size(), 1);
    QCOMPARE(layout.glyphCells.constFirst().text, QString::fromUtf8(u8"≡"));
}

void TerminalRowLayoutTest::raisesTruecolorForegroundToReadableContrast() {
    TerminalFontSet fonts;
    QFont font(QStringLiteral("Consolas"));
    font.setPixelSize(14);
    fonts.setFont(font);

    Line line;
    line.cells = {cell(QStringLiteral("x"))};
    line.cells[0].style.defaultForeground = false;
    line.cells[0].style.foreground = QColor(QStringLiteral("#d0d0d0"));
    const QColor background(QStringLiteral("#fefefe"));
    const TerminalRowLayout layout = TerminalRowLayoutBuilder::build(
        line, fonts, QColor(QStringLiteral("#252532")), background, {});

    QCOMPARE(layout.glyphCells.size(), 1);
    const QColor painted = layout.glyphCells.constFirst().color;
    QVERIFY(contrastRatio(painted, background) >= kMinimumReadableContrast);
    QVERIFY(painted != QColor(QStringLiteral("#d0d0d0")));
}

void TerminalRowLayoutTest::leavesReadableThemeForegroundUnchanged() {
    TerminalFontSet fonts;
    QFont font(QStringLiteral("Consolas"));
    font.setPixelSize(14);
    fonts.setFont(font);

    Line line;
    line.cells = {cell(QStringLiteral("x"))};
    const QColor foreground(QStringLiteral("#252532"));
    const QColor background(QStringLiteral("#fefefe"));
    const TerminalRowLayout layout = TerminalRowLayoutBuilder::build(
        line, fonts, foreground, background, {});

    QCOMPARE(layout.glyphCells.size(), 1);
    QCOMPARE(layout.glyphCells.constFirst().color, foreground);
}

void TerminalRowLayoutTest::leavesDimBlockChromeUnchanged() {
    TerminalFontSet fonts;
    QFont font(QStringLiteral("Consolas"));
    font.setPixelSize(14);
    fonts.setFont(font);

    Line line;
    line.cells = {cell(QString::fromUtf8(u8"█"))};
    line.cells[0].style.defaultForeground = false;
    const QColor fill(QStringLiteral("#303030"));
    line.cells[0].style.foreground = fill;
    const QColor background(QStringLiteral("#090b13"));
    const TerminalRowLayout layout = TerminalRowLayoutBuilder::build(
        line, fonts, Qt::white, background, {});

    QCOMPARE(layout.customGlyphs.size(), 1);
    QCOMPARE(layout.customGlyphs.constFirst().color, fill);
}

void TerminalRowLayoutTest::raisesDefaultInkOnDarkComposerBar() {
    TerminalFontSet fonts;
    QFont font(QStringLiteral("Consolas"));
    font.setPixelSize(14);
    fonts.setFont(font);

    Line line;
    line.cells = {cell(QStringLiteral("S"))};
    line.cells[0].style.defaultBackground = false;
    line.cells[0].style.background = QColor(QStringLiteral("#1e1e1e"));
    const QColor lightThemeInk(QStringLiteral("#252532"));
    const QColor composer(QStringLiteral("#1e1e1e"));
    const TerminalRowLayout layout = TerminalRowLayoutBuilder::build(
        line, fonts, lightThemeInk, QColor(QStringLiteral("#fefefe")), {});

    QCOMPARE(layout.glyphCells.size(), 1);
    const QColor painted = layout.glyphCells.constFirst().color;
    QVERIFY(contrastRatio(lightThemeInk, composer) < kMinimumReadableContrast);
    QVERIFY(contrastRatio(painted, composer) >= kMinimumReadableContrast);
    QVERIFY(relativeLuminance(painted) > relativeLuminance(lightThemeInk));
}

void TerminalRowLayoutTest::raisesAnsiDimOnDarkComposerBar() {
    TerminalFontSet fonts;
    QFont font(QStringLiteral("Consolas"));
    font.setPixelSize(14);
    fonts.setFont(font);

    const QColor dim = defaultAnsiPalette(false).at(8);
    const QColor composer(QStringLiteral("#1e1e1e"));
    Line line;
    line.cells = {cell(QStringLiteral("S"))};
    line.cells[0].style.defaultForeground = false;
    line.cells[0].style.foregroundPaletteIndex = 8;
    line.cells[0].style.defaultBackground = false;
    line.cells[0].style.background = composer;
    const TerminalRowLayout layout = TerminalRowLayoutBuilder::build(
        line, fonts, defaultSurfaceForeground(false),
        defaultSurfaceBackground(false), defaultAnsiPalette(false));

    QCOMPARE(layout.glyphCells.size(), 1);
    const QColor painted = layout.glyphCells.constFirst().color;
    QVERIFY(contrastRatio(dim, composer) < kMinimumReadableContrast);
    QVERIFY(contrastRatio(painted, composer) >= kMinimumReadableContrast);
}

void TerminalRowLayoutTest::fadesFaintInkTowardTheBackground() {
    TerminalFontSet fonts;
    QFont font(QStringLiteral("Consolas"));
    font.setPixelSize(14);
    fonts.setFont(font);

    Line line;
    line.cells = {cell(QStringLiteral("a")), cell(QStringLiteral("b"))};
    line.cells[1].style.faint = true;
    const QColor foreground(QStringLiteral("#202020"));
    const QColor background(QStringLiteral("#e0e0e0"));
    const TerminalRowLayout layout = TerminalRowLayoutBuilder::build(
        line, fonts, foreground, background, {});

    QCOMPARE(layout.glyphCells.size(), 2);
    QCOMPARE(layout.glyphCells.at(0).color, foreground);
    // Halfway to the background: contrast repair must not undo SGR 2.
    QCOMPARE(layout.glyphCells.at(1).color, QColor(QStringLiteral("#808080")));
}

void TerminalRowLayoutTest::resolvesUnderlineColorAndOverline() {
    TerminalFontSet fonts;
    QFont font(QStringLiteral("Consolas"));
    font.setPixelSize(14);
    fonts.setFont(font);

    Line line;
    line.cells = {cell(QStringLiteral("a")), cell(QStringLiteral("b")),
                  cell(QStringLiteral("c")), cell(QStringLiteral("d"))};
    styleColumns(line, 0, 3, [](CellStyle &style) { style.underline = UnderlineStyle::Curly; });
    line.cells[1].style.underlineColor = QColor(QStringLiteral("#ff0000"));
    line.cells[2].style.underlineColorPaletteIndex = 3;
    line.cells[3].style.overline = true;
    const QColor foreground(QStringLiteral("#202020"));
    QVector<QColor> palette(16, QColor(QStringLiteral("#000000")));
    palette[3] = QColor(QStringLiteral("#806000"));
    const TerminalRowLayout layout = TerminalRowLayoutBuilder::build(
        line, fonts, foreground, QColor(QStringLiteral("#f0f0f0")), palette);

    QCOMPARE(layout.decorations.size(), 4);
    // Without SGR 58 the underline follows the text.
    QCOMPARE(layout.decorations.at(0).underlineColor, foreground);
    QCOMPARE(layout.decorations.at(1).underlineColor, QColor(QStringLiteral("#ff0000")));
    QCOMPARE(layout.decorations.at(1).color, foreground);
    QCOMPARE(layout.decorations.at(2).underlineColor, QColor(QStringLiteral("#806000")));
    QVERIFY(layout.decorations.at(3).overline);
    QCOMPARE(layout.decorations.at(3).underline, UnderlineStyle::None);
}

QTEST_MAIN(TerminalRowLayoutTest)

#include "terminal_row_layout_test.moc"
