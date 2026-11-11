#include "terminal/api/terminal_types.h"
#include "terminal/render/terminal_drawn_glyphs.h"
#include "terminal/render/terminal_font_set.h"
#include "terminal/render/terminal_row_layout.h"

#include <QTest>

using namespace span::terminal;

class TerminalBlockElementsTest final : public QObject {
    Q_OBJECT

private slots:
    void fillsCellsWithoutFontPadding();
    void supportsOpenCodeBlockAlphabet();
    void routesBlockElementsThroughCustomGlyphLayout();
    void drawsSextantsAsSixths();
};

void TerminalBlockElementsTest::fillsCellsWithoutFontPadding() {
    const QRectF cell(10, 20, 8, 16);
    const QVector<QRectF> full = TerminalBlockElements::rectangles(QString::fromUtf8(u8"█"), cell, 1.0);
    QCOMPARE(full.size(), 1);
    QCOMPARE(full.first(), cell);

    const QVector<QRectF> upper = TerminalBlockElements::rectangles(QString::fromUtf8(u8"▀"), cell, 1.0);
    QCOMPARE(upper.size(), 1);
    QCOMPARE(upper.first(), QRectF(10, 20, 8, 8));

    const QVector<QRectF> lower = TerminalBlockElements::rectangles(QString::fromUtf8(u8"▄"), cell, 1.0);
    QCOMPARE(lower.size(), 1);
    QCOMPARE(lower.first(), QRectF(10, 28, 8, 8));
}

void TerminalBlockElementsTest::supportsOpenCodeBlockAlphabet() {
    QVERIFY(TerminalBlockElements::supports(QString::fromUtf8(u8"█")));
    QVERIFY(TerminalBlockElements::supports(QString::fromUtf8(u8"▀")));
    QVERIFY(TerminalBlockElements::supports(QString::fromUtf8(u8"▄")));
    QVERIFY(TerminalBlockElements::supports(QString::fromUtf8(u8"▌")));
    QVERIFY(TerminalBlockElements::supports(QString::fromUtf8(u8"▐")));
}

void TerminalBlockElementsTest::routesBlockElementsThroughCustomGlyphLayout() {
    Line line;
    line.cells = {Cell {QString::fromUtf8(u8"█")}, Cell {QString::fromUtf8(u8"▄")}};
    TerminalFontSet fonts;
    QFont font(QStringLiteral("JetBrains Mono"));
    font.setPixelSize(14);
    fonts.setFont(font);

    const TerminalRowLayout layout = TerminalRowLayoutBuilder::build(line, fonts, Qt::black, Qt::white, {});
    QCOMPARE(layout.customGlyphs.size(), 2);
    QCOMPARE(layout.glyphCells.size(), 0);
}

void TerminalBlockElementsTest::drawsSextantsAsSixths() {
    const QRectF cell(0, 0, 8, 12);
    const auto sextant = [](char32_t codepoint) { return QString::fromUcs4(&codepoint, 1); };

    // U+1FB00 is the top-left sixth alone.
    QCOMPARE(TerminalBlockElements::rectangles(sextant(0x1fb00), cell, 1.0),
             QVector<QRectF>({QRectF(0, 0, 4, 4)}));
    // U+1FB14 (SEXTANT-235) is the first after the skipped mask 21 (U+258C).
    QCOMPARE(TerminalBlockElements::rectangles(sextant(0x1fb14), cell, 1.0),
             QVector<QRectF>({QRectF(4, 0, 4, 4), QRectF(0, 4, 4, 4), QRectF(0, 8, 4, 4)}));
    // U+1FB3B is the last one: everything but the top-left sixth.
    QCOMPARE(TerminalBlockElements::rectangles(sextant(0x1fb3b), cell, 1.0),
             QVector<QRectF>({QRectF(4, 0, 4, 4), QRectF(0, 4, 8, 4), QRectF(0, 8, 8, 4)}));
    QVERIFY(!TerminalBlockElements::supports(sextant(0x1fb3c)));
}

QTEST_MAIN(TerminalBlockElementsTest)

#include "terminal_block_elements_test.moc"
