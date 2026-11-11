#include "terminal/core/terminal_color.h"

#include <QJsonArray>
#include <QJsonObject>
#include <QSet>
#include <QTest>

#include <algorithm>

using namespace span::terminal;

namespace {

// What is missing from each scheme. State layers are translucent overlays;
// an opaque or empty one would hide or erase whatever surface they land on.
QStringList schemeProblems(const QVector<TerminalColorScheme> &schemes)
{
    QStringList problems;
    for (const TerminalColorScheme &scheme : schemes) {
        const auto note = [&](bool ok, const char *what) {
            if (!ok) problems.append(QStringLiteral("%1: %2").arg(scheme.name, QLatin1String(what)));
        };
        note(!scheme.name.isEmpty(), "no name");
        note(scheme.isValid(), "incomplete");
        note(scheme.uiBackground.isValid(), "no UI paper");
        note(scheme.uiPlate.isValid(), "no UI plate");
        note(scheme.uiAccent.isValid(), "no UI accent");
        for (const QColor &layer : {scheme.uiHover, scheme.uiSelection, scheme.uiPressed})
            note(layer.isValid() && layer.alpha() > 0 && layer.alpha() < 255, "bad state layer");
    }
    return problems;
}

QStringList schemeNames(const QVector<TerminalColorScheme> &schemes)
{
    QStringList names;
    for (const TerminalColorScheme &scheme : schemes) names.append(scheme.name);
    return names;
}

const TerminalColorScheme *schemeNamed(const QVector<TerminalColorScheme> &schemes, const QString &name)
{
    const auto found = std::find_if(schemes.cbegin(), schemes.cend(),
                                    [&](const TerminalColorScheme &scheme) { return scheme.name == name; });
    return found == schemes.cend() ? nullptr : &*found;
}

} // namespace

class TerminalColorTest final : public QObject {
    Q_OBJECT

private slots:
    void reportsLightColorFgBgForLightBackground();
    void reportsDarkColorFgBgForDarkBackground();
    void reportsDarkColorFgBgWhenBackgroundUnknown();
    void leavesReadableTruecolorUnchanged();
    void darkensLightGrayOnLightBackground();
    void lightensDarkGrayOnDarkBackground();
    void preservesTransparentForeground();
    void builtinSchemesAreCompleteAndUnique();
    void spanSchemesReproduceDefaultSurface();
    void schemeJsonRoundTripsColors();
    void schemeJsonFillsMissingUiPaperFromBackground();
    void schemeJsonRejectsIncompleteSchemes();
};

void TerminalColorTest::reportsLightColorFgBgForLightBackground() {
    QCOMPARE(colorFgBg(defaultSurfaceForeground(false),
                       defaultSurfaceBackground(false)),
             QStringLiteral("0;15"));
}

void TerminalColorTest::reportsDarkColorFgBgForDarkBackground() {
    QCOMPARE(colorFgBg(defaultSurfaceForeground(true),
                       defaultSurfaceBackground(true)),
             QStringLiteral("15;0"));
}

void TerminalColorTest::reportsDarkColorFgBgWhenBackgroundUnknown() {
    QCOMPARE(colorFgBg(QColor(QStringLiteral("#202020")), QColor()),
             QStringLiteral("15;0"));
    QCOMPARE(colorFgBg(QColor(QStringLiteral("#202020")), Qt::transparent),
             QStringLiteral("15;0"));
}

void TerminalColorTest::leavesReadableTruecolorUnchanged() {
    const QColor foreground(QStringLiteral("#252532"));
    const QColor background(QStringLiteral("#fefefe"));
    QCOMPARE(ensureMinimumContrast(foreground, background), foreground);
}

void TerminalColorTest::darkensLightGrayOnLightBackground() {
    const QColor foreground(QStringLiteral("#d0d0d0"));
    const QColor background(QStringLiteral("#fefefe"));
    QVERIFY(contrastRatio(foreground, background) < kMinimumReadableContrast);

    const QColor adjusted = ensureMinimumContrast(foreground, background);
    QVERIFY(contrastRatio(adjusted, background) >= kMinimumReadableContrast);
    QVERIFY(relativeLuminance(adjusted) < relativeLuminance(foreground));
}

void TerminalColorTest::lightensDarkGrayOnDarkBackground() {
    const QColor foreground(QStringLiteral("#303030"));
    const QColor background(QStringLiteral("#090b13"));
    QVERIFY(contrastRatio(foreground, background) < kMinimumReadableContrast);

    const QColor adjusted = ensureMinimumContrast(foreground, background);
    QVERIFY(contrastRatio(adjusted, background) >= kMinimumReadableContrast);
    QVERIFY(relativeLuminance(adjusted) > relativeLuminance(foreground));
}

void TerminalColorTest::preservesTransparentForeground() {
    const QColor foreground(0, 0, 0, 0);
    QCOMPARE(ensureMinimumContrast(foreground, QColor(QStringLiteral("#fefefe"))),
             foreground);
}

void TerminalColorTest::builtinSchemesAreCompleteAndUnique() {
    const QVector<TerminalColorScheme> schemes = builtinColorSchemes();
    QCOMPARE(schemeProblems(schemes), QStringList());
    const QStringList names = schemeNames(schemes);
    QCOMPARE(QSet<QString>(names.cbegin(), names.cend()).size(), names.size());
    // The default dark and light slots name these schemes.
    QVERIFY(names.contains(QStringLiteral("Span Dark")));
    QVERIFY(names.contains(QStringLiteral("Span Light")));
}

void TerminalColorTest::spanSchemesReproduceDefaultSurface() {
    const QVector<TerminalColorScheme> schemes = builtinColorSchemes();
    const TerminalColorScheme *dark = schemeNamed(schemes, QStringLiteral("Span Dark"));
    const TerminalColorScheme *light = schemeNamed(schemes, QStringLiteral("Span Light"));
    QVERIFY(dark);
    QVERIFY(light);
    QCOMPARE(dark->foreground, defaultSurfaceForeground(true));
    QCOMPARE(dark->background, defaultSurfaceBackground(true));
    QCOMPARE(dark->ansi, defaultAnsiPalette(true));
    QVERIFY(dark->cursorColor.isValid());
    QVERIFY(dark->selectionBackground.isValid());
    QCOMPARE(dark->uiBackground, dark->background);
    // Span selects with a soft accent tint; hover stays neutral ink.
    QCOMPARE(dark->uiSelection.rgb(), dark->uiAccent.rgb());
    QCOMPARE(dark->uiHover.rgb(), dark->foreground.rgb());

    QCOMPARE(light->foreground, defaultSurfaceForeground(false));
    QCOMPARE(light->background, defaultSurfaceBackground(false));
    QCOMPARE(light->ansi, defaultAnsiPalette(false));
    QVERIFY(light->cursorColor.isValid());
    QVERIFY(light->selectionBackground.isValid());
    QCOMPARE(light->uiBackground, light->background);
    QCOMPARE(light->uiSelection.rgb(), light->uiAccent.rgb());
    QVERIFY(light->uiPlate != light->uiBackground);
}

void TerminalColorTest::schemeJsonRoundTripsColors() {
    const QVector<TerminalColorScheme> schemes = builtinColorSchemes();
    QVERIFY(!schemes.isEmpty());
    TerminalColorScheme scheme = schemes.constFirst();
    scheme.name = QStringLiteral("Workbench");
    scheme.selectionBackground = QColor(QStringLiteral("#80ff0000"));
    scheme.uiHover = QColor(QStringLiteral("#14b45a1c"));
    scheme.uiSelection = QColor(QStringLiteral("#30b45a1c"));
    scheme.uiPressed = QColor(QStringLiteral("#40252532"));

    const QJsonObject json = schemeToJson(scheme);
    TerminalColorScheme parsed;
    QVERIFY(schemeFromJson(scheme.name, json, &parsed));
    QCOMPARE(parsed.name, scheme.name);
    QCOMPARE(parsed.foreground, scheme.foreground);
    QCOMPARE(parsed.background, scheme.background);
    QCOMPARE(parsed.cursorColor, scheme.cursorColor);
    QCOMPARE(parsed.selectionBackground, scheme.selectionBackground);
    QCOMPARE(parsed.selectionBackground.alpha(), 128);
    QCOMPARE(parsed.ansi, scheme.ansi);
    QCOMPARE(parsed.uiBackground, scheme.uiBackground);
    QCOMPARE(parsed.uiPlate, scheme.uiPlate);
    QCOMPARE(parsed.uiAccent, scheme.uiAccent);
    QCOMPARE(parsed.uiHover, scheme.uiHover);
    QCOMPARE(parsed.uiSelection, scheme.uiSelection);
    QCOMPARE(parsed.uiSelection.alpha(), 0x30);
    QCOMPARE(parsed.uiPressed, scheme.uiPressed);
}

void TerminalColorTest::schemeJsonFillsMissingUiPaperFromBackground() {
    const QVector<TerminalColorScheme> schemes = builtinColorSchemes();
    QVERIFY(!schemes.isEmpty());
    QJsonObject json = schemeToJson(schemes.constFirst());
    json.remove(QStringLiteral("uiBackground"));
    json.remove(QStringLiteral("uiPlate"));
    json.remove(QStringLiteral("uiAccent"));
    json.remove(QStringLiteral("uiHover"));
    json.remove(QStringLiteral("uiSelection"));
    json.remove(QStringLiteral("uiPressed"));
    TerminalColorScheme parsed;
    QVERIFY(schemeFromJson(QStringLiteral("Derived"), json, &parsed));
    QCOMPARE(parsed.uiBackground, parsed.background);
    QVERIFY(parsed.uiPlate.isValid());
    QVERIFY(parsed.uiAccent.isValid());
    // Legacy schemes get neutral layers: the foreground hue, selected at
    // least as strong as hover.
    QCOMPARE(parsed.uiHover.rgb(), parsed.foreground.rgb());
    QCOMPARE(parsed.uiSelection.rgb(), parsed.foreground.rgb());
    QCOMPARE(parsed.uiPressed.rgb(), parsed.foreground.rgb());
    QVERIFY(parsed.uiHover.alpha() > 0);
    QVERIFY(parsed.uiSelection.alpha() > parsed.uiHover.alpha());
}

void TerminalColorTest::schemeJsonRejectsIncompleteSchemes() {
    const QVector<TerminalColorScheme> schemes = builtinColorSchemes();
    QVERIFY(!schemes.isEmpty());

    QJsonObject broken = schemeToJson(schemes.constFirst());
    broken.remove(QStringLiteral("cursor"));
    TerminalColorScheme parsed;
    QVERIFY(!schemeFromJson(QStringLiteral("Broken"), broken, &parsed));

    QJsonObject shortAnsi = schemeToJson(schemes.constFirst());
    QJsonArray ansi = shortAnsi.value(QStringLiteral("ansi")).toArray();
    ansi.removeLast();
    shortAnsi.insert(QStringLiteral("ansi"), ansi);
    QVERIFY(!schemeFromJson(QStringLiteral("Short"), shortAnsi, &parsed));

    QVERIFY(!schemeFromJson(QStringLiteral("Empty"), QJsonObject(), &parsed));
}

QTEST_MAIN(TerminalColorTest)
#include "terminal_color_test.moc"
