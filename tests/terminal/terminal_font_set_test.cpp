#include "terminal/render/terminal_font_set.h"

#include <QFileInfo>
#include <QFontDatabase>
#include <QFontMetricsF>
#include <QRawFont>
#include <QTest>

#include <algorithm>
#include <cmath>

using namespace span::terminal;

class TerminalFontSetTest final : public QObject {
    Q_OBJECT

private slots:
    void initTestCase();
    void resolvesFirstInstalledFixedPitchFamily();
    void fallsBackToBundledFixedFont();
    void preservesInstalledNonFixedFallback();
    void keepsRegularWhenBoldFaceIsUnavailable();
    void normalizesConfiguredWeightToNativeFace();
    void bundledFaceDrawsReplacementCharacter();
    void usesOneStableCellMetricForEveryGlyph();
    void reportsResolvedGridMetrics();
    void usesNaturalPhysicalVerticalMetrics_data();
    void usesNaturalPhysicalVerticalMetrics();

private:
    int m_regularFontId = -1;
    QString m_bundledFamily;
    QString m_symbolsFamily;
};

void TerminalFontSetTest::initTestCase() {
    const QString regularPath =
        QStringLiteral(SPANTUI_SOURCE_DIR
                       "/assets/fonts/SpanMono-Regular.ttf");
    QVERIFY(QFileInfo::exists(regularPath));
    m_regularFontId = QFontDatabase::addApplicationFont(regularPath);
    QVERIFY(m_regularFontId >= 0);
    const QStringList families = QFontDatabase::applicationFontFamilies(m_regularFontId);
    QVERIFY(!families.isEmpty());
    m_bundledFamily = families.constFirst();
    QVERIFY(QFontDatabase::isFixedPitch(m_bundledFamily));

    const int symbolsId = QFontDatabase::addApplicationFont(
        QStringLiteral(SPANTUI_SOURCE_DIR "/assets/fonts/SpanSymbols-Regular.ttf"));
    QVERIFY(symbolsId >= 0);
    m_symbolsFamily = QFontDatabase::applicationFontFamilies(symbolsId).value(0);
    QCOMPARE(m_symbolsFamily, QStringLiteral("Span Symbols"));
}

void TerminalFontSetTest::resolvesFirstInstalledFixedPitchFamily() {
    const QString systemFixedFamily = TerminalFontSet::safeFixedFont().family();
    QFont font;
    font.setFamilies({QStringLiteral("SpanTui Missing Mono"), systemFixedFamily});
    font.setPixelSize(15);

    TerminalFontSet fonts;
    fonts.setFont(font);
    QCOMPARE(fonts.baseFont().family(), systemFixedFamily);
    QCOMPARE(fonts.baseFont().pixelSize(), 15);
    QVERIFY(fonts.baseFont().fixedPitch());
}

void TerminalFontSetTest::fallsBackToBundledFixedFont() {
    QFont font;
    font.setFamilies({QStringLiteral("SpanTui Missing Mono"), QStringLiteral("SpanTui Missing CJK")});
    font.setPixelSize(14);

    TerminalFontSet fonts;
    fonts.setFont(font);

    const QStringList resolved = fonts.baseFont().families();
    QCOMPARE(resolved.constFirst(), m_bundledFamily);
    // Nerd icons come from the shared symbols face right after Span Mono.
    QCOMPARE(resolved.value(1), m_symbolsFamily);
    QCOMPARE(fonts.baseFont().pixelSize(), 14);
    QVERIFY(fonts.baseFont().fixedPitch());
}

void TerminalFontSetTest::preservesInstalledNonFixedFallback() {
    const QStringList families = QFontDatabase::families();
    const auto proportional = std::find_if(families.cbegin(), families.cend(),
                                           [](const QString &family) { return !QFontDatabase::isFixedPitch(family); });
    const QString nonFixedFamily = proportional == families.cend() ? QString() : *proportional;
    QVERIFY2(!nonFixedFamily.isEmpty(), "No installed proportional font is available for fallback testing.");

    QFont font;
    font.setFamilies({QStringLiteral("SpanTui Missing Mono"), nonFixedFamily});
    font.setPixelSize(14);

    TerminalFontSet fonts;
    fonts.setFont(font);

    const QStringList resolved = fonts.baseFont().families();
    QCOMPARE(resolved.constFirst(), m_bundledFamily);
    QVERIFY(resolved.contains(nonFixedFamily, Qt::CaseInsensitive));
    QVERIFY(resolved.indexOf(nonFixedFamily) > 0);
}

void TerminalFontSetTest::keepsRegularWhenBoldFaceIsUnavailable() {
    QFont font(m_bundledFamily);
    font.setPixelSize(14);
    TerminalFontSet fonts;
    fonts.setFont(font);
    QVERIFY(!fonts.boldFaceAvailable());

    CellStyle bold;
    bold.bold = true;
    QVERIFY(!fonts.fontForStyle(bold).bold());
}

void TerminalFontSetTest::normalizesConfiguredWeightToNativeFace() {
    QFont font(m_bundledFamily);
    font.setPixelSize(14);
    font.setWeight(QFont::Normal);
    TerminalFontSet fonts;
    fonts.setFont(font);
    CellStyle regular;
    QCOMPARE(fonts.fontForStyle(regular).weight(), QFont::Normal);
    QCOMPARE(QRawFont::fromFont(fonts.fontForStyle(regular)).familyName(), m_bundledFamily);
    QVERIFY(!fonts.fontForStyle(regular).styleName().isEmpty());

    QFont lightFont(m_bundledFamily);
    lightFont.setPixelSize(14);
    lightFont.setWeight(QFont::Light);
    fonts.setFont(lightFont);
    QCOMPARE(fonts.fontForStyle(regular).weight(), QFont::Normal);
}

void TerminalFontSetTest::bundledFaceDrawsReplacementCharacter()
{
    // Undecodable output arrives as U+FFFD. Span Mono draws it on its own
    // cell, so Qt never searches every system font for the glyph.
    QFont font(m_bundledFamily);
    font.setPixelSize(14);
    TerminalFontSet fonts;
    fonts.setFont(font);

    const QRawFont raw = QRawFont::fromFont(fonts.baseFont());
    QCOMPARE(raw.familyName(), m_bundledFamily);
    QVERIFY(raw.supportsCharacter(QChar(0xFFFD)));
    const QList<quint32> glyphs = raw.glyphIndexesForString(QString(QChar(0xFFFD)) + QLatin1Char('M'));
    QCOMPARE(glyphs.size(), 2);
    QVERIFY(glyphs.at(0) != 0);
    const QList<QPointF> advances = raw.advancesForGlyphIndexes(glyphs);
    QCOMPARE(advances.at(0).x(), advances.at(1).x());
}

void TerminalFontSetTest::usesOneStableCellMetricForEveryGlyph() {
    QFont font;
    font.setFamilies({QStringLiteral("Consolas"), QStringLiteral("Microsoft YaHei Mono")});
    font.setPixelSize(14);

    TerminalFontSet fonts;
    fonts.setFont(font);
    fonts.setDevicePixelRatio(1.5);
    const qreal width = fonts.cellWidth();
    const qreal height = fonts.cellHeight();

    // Resolving styled faces never changes the grid every glyph shares.
    CellStyle bold;
    bold.bold = true;
    CellStyle italic;
    italic.italic = true;
    const QFont boldFont = fonts.fontForStyle(bold);
    fonts.fontForStyle(italic);
    QCOMPARE(fonts.cellWidth(), width);
    QCOMPARE(fonts.cellHeight(), height);
    QCOMPARE(boldFont.family(), fonts.baseFont().family());
    QVERIFY(width > 0);
    QVERIFY(height > 0);
    // Consolas ships a real bold face, so bold is not synthesized.
    if (!fonts.boldFaceAvailable()) QSKIP("Consolas bold face unavailable; real bold not verified.");
    QVERIFY(boldFont.bold());
}

void TerminalFontSetTest::reportsResolvedGridMetrics() {
    const QStringList fixedPitchFamilies = TerminalFontSet::availableFamilies(true);
    const QStringList allFamilies = TerminalFontSet::availableFamilies(false);
    QVERIFY(!fixedPitchFamilies.isEmpty());
    QVERIFY(!allFamilies.contains(QStringLiteral("MS Sans Serif"), Qt::CaseInsensitive));
    QVERIFY(!allFamilies.contains(QStringLiteral("Fixedsys"), Qt::CaseInsensitive));
    QVERIFY(!allFamilies.contains(QStringLiteral("Roman"), Qt::CaseInsensitive));
    QVERIFY(!allFamilies.contains(QStringLiteral("Swiss"), Qt::CaseInsensitive));
    QVERIFY(!allFamilies.contains(QStringLiteral("Modern"), Qt::CaseInsensitive));
    QVERIFY(!allFamilies.contains(QStringLiteral("Script"), Qt::CaseInsensitive));
    QVERIFY(!allFamilies.contains(QStringLiteral("Decorative"), Qt::CaseInsensitive));
    QVERIFY(!allFamilies.contains(QStringLiteral("Terminal"), Qt::CaseInsensitive));
    QVERIFY(!allFamilies.contains(m_symbolsFamily, Qt::CaseInsensitive));

    QFont font;
    font.setFamilies({fixedPitchFamilies.first()});
    font.setPixelSize(14);
    TerminalFontSet fonts;
    fonts.setFont(font);

    const QFont resolved = fonts.baseFont();
    QVERIFY(QFontDatabase::isFixedPitch(resolved.families().constFirst()));
    const QFontMetricsF metrics(resolved);
    const qreal mAdvance = metrics.horizontalAdvance(QStringLiteral("M"));
    const qreal iAdvance = metrics.horizontalAdvance(QStringLiteral("i"));
    const qreal zeroAdvance = metrics.horizontalAdvance(QStringLiteral("0"));
    QVERIFY(qAbs(mAdvance - iAdvance) < 0.01);
    QVERIFY(qAbs(mAdvance - zeroAdvance) < 0.01);
}

void TerminalFontSetTest::usesNaturalPhysicalVerticalMetrics_data() {
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

void TerminalFontSetTest::usesNaturalPhysicalVerticalMetrics() {
    QFETCH(int, pixelSize);
    QFETCH(qreal, ratio);

    TerminalFontSet fonts;
    fonts.setFont(TerminalFontSet::safeFixedFont(pixelSize));
    fonts.setDevicePixelRatio(ratio);
    const QFontMetricsF metrics(fonts.baseFont());
    const qreal ascent = std::max<qreal>(0.0, metrics.ascent()) * ratio;
    const qreal descent = std::max<qreal>(0.0, metrics.descent()) * ratio;
    const qreal leading = std::max<qreal>(0.0, metrics.leading()) * ratio;
    const qreal physicalHeight = fonts.cellHeight() * ratio;
    const qreal physicalBaseline = fonts.baseline() * ratio;

    // The grid sits on whole device pixels.
    QCOMPARE(physicalHeight, std::round(physicalHeight));
    QCOMPARE(physicalBaseline, std::round(physicalBaseline));
    // Every glyph's ascent and descent fit inside its cell.
    QVERIFY(physicalBaseline - ascent >= -0.001);
    QVERIFY(physicalBaseline + descent <= physicalHeight + 0.001);
    // Natural height: the font's own line box, rounded up to device pixels
    // (at most one pixel per rounded metric), with no extra padding.
    QVERIFY(physicalHeight < ascent + descent + leading + 3.0);
}

QTEST_MAIN(TerminalFontSetTest)

#include "terminal_font_set_test.moc"
