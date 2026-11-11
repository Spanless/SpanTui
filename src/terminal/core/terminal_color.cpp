#include "terminal/core/terminal_color.h"

#include <algorithm>
#include <cmath>
#include <initializer_list>

namespace span::terminal {
namespace {

qreal linearChannel(qreal channel) {
    return channel <= 0.04045 ? channel / 12.92
                              : std::pow((channel + 0.055) / 1.055, 2.4);
}

QColor mixRgb(const QColor &from, const QColor &to, qreal amount) {
    const qreal t = std::clamp(amount, 0.0, 1.0);
    return QColor::fromRgbF(from.redF() + (to.redF() - from.redF()) * t,
                            from.greenF() + (to.greenF() - from.greenF()) * t,
                            from.blueF() + (to.blueF() - from.blueF()) * t,
                            from.alphaF());
}

QVector<QColor> paletteFromHex(std::initializer_list<const char *> hexes) {
    QVector<QColor> colors;
    colors.reserve(int(hexes.size()));
    for (const char *hex : hexes) {
        colors.append(QColor(QLatin1String(hex)));
    }
    return colors;
}

} // namespace

qreal relativeLuminance(const QColor &color) {
    if (!color.isValid()) {
        return 0.0;
    }
    const QColor rgb = color.toRgb();
    return 0.2126 * linearChannel(rgb.redF()) + 0.7152 * linearChannel(rgb.greenF())
           + 0.0722 * linearChannel(rgb.blueF());
}

qreal contrastRatio(const QColor &first, const QColor &second) {
    const qreal lighter = std::max(relativeLuminance(first), relativeLuminance(second));
    const qreal darker = std::min(relativeLuminance(first), relativeLuminance(second));
    return (lighter + 0.05) / (darker + 0.05);
}

bool isLightSurface(const QColor &background) {
    return background.isValid() && background.alpha() > 0
           && relativeLuminance(background) > 0.5;
}

QString colorFgBg(const QColor &foreground, const QColor &background) {
    Q_UNUSED(foreground);
    return isLightSurface(background) ? QStringLiteral("0;15")
                                      : QStringLiteral("15;0");
}

QColor ensureMinimumContrast(const QColor &foreground,
                             const QColor &background,
                             qreal minimumRatio) {
    if (!foreground.isValid() || foreground.alpha() <= 0 || !background.isValid()
        || background.alpha() <= 0 || minimumRatio <= 1.0) {
        return foreground;
    }
    if (contrastRatio(foreground, background) >= minimumRatio) {
        return foreground;
    }

    const QColor black = QColor::fromRgb(0, 0, 0, foreground.alpha());
    const QColor white = QColor::fromRgb(255, 255, 255, foreground.alpha());
    const bool towardBlack = contrastRatio(black, background) >= contrastRatio(white, background);
    const QColor target = towardBlack ? black : white;
    if (contrastRatio(target, background) < minimumRatio) {
        return target;
    }

    qreal low = 0.0;
    qreal high = 1.0;
    QColor best = target;
    for (int step = 0; step < 14; ++step) {
        const qreal mid = (low + high) * 0.5;
        const QColor candidate = mixRgb(foreground, target, mid);
        if (contrastRatio(candidate, background) >= minimumRatio) {
            best = candidate;
            high = mid;
        } else {
            low = mid;
        }
    }
    return best;
}

QColor defaultSurfaceForeground(bool dark) {
    return QColor(QLatin1String(dark ? "#ebe7df" : "#26231e"));
}

QColor defaultSurfaceBackground(bool dark) {
    return QColor(QLatin1String(dark ? "#131211" : "#f5f3ef"));
}

QVector<QColor> defaultAnsiPalette(bool dark) {
    if (dark) {
        return paletteFromHex({
            "#2b2926", "#f06b7a", "#5ed4a0", "#f4c36a",
            "#75a7ff", "#c596ff", "#58cbd4", "#e4e0d9",
            "#76716a", "#ff8d99", "#78e2b4", "#ffd37d",
            "#95bbff", "#d0b1ff", "#7de1e8", "#ffffff",
        });
    }
    return paletteFromHex({
        "#34312c", "#b4233c", "#147a52", "#8a5a00",
        "#285eab", "#6d3fa3", "#0f6f7a", "#625d55",
        "#56514a", "#d03a4c", "#168a5b", "#8a5a00",
        "#1e5fc0", "#7b45ba", "#0c7d89", "#211e1a",
    });
}

bool TerminalColorScheme::isValid() const {
    if (ansi.size() != 16) {
        return false;
    }
    return foreground.isValid() && background.isValid() && cursorColor.isValid()
           && selectionBackground.isValid()
           && std::all_of(ansi.cbegin(), ansi.cend(),
                          [](const QColor &color) { return color.isValid(); });
}

void completeSchemeUi(TerminalColorScheme *scheme) {
    if (scheme == nullptr || !scheme->isValid()) {
        return;
    }
    if (!scheme->uiBackground.isValid()) {
        scheme->uiBackground = scheme->background;
    }
    if (!scheme->uiPlate.isValid()) {
        scheme->uiPlate = scheme->uiBackground;
    }
    if (!scheme->uiAccent.isValid()) {
        if (scheme->ansi.size() == 16) {
            scheme->uiAccent = isLightSurface(scheme->uiBackground)
                                   ? scheme->ansi.at(4)
                                   : scheme->ansi.at(12);
        } else {
            scheme->uiAccent = isLightSurface(scheme->uiBackground)
                                   ? QColor(QLatin1String("#3d4451"))
                                   : QColor(QLatin1String("#c2c9d6"));
        }
    }
    // Neutral state layers: the foreground at a strength tuned to the plate.
    // Pressed matches selected so a press previews the selection it commits.
    const bool lightPlate = isLightSurface(scheme->uiPlate);
    const auto layer = [scheme](qreal strength) {
        QColor color = scheme->foreground;
        color.setAlphaF(strength);
        return color;
    };
    if (!scheme->uiHover.isValid()) {
        scheme->uiHover = layer(lightPlate ? 0.06 : 0.10);
    }
    if (!scheme->uiSelection.isValid()) {
        scheme->uiSelection = layer(lightPlate ? 0.11 : 0.16);
    }
    if (!scheme->uiPressed.isValid()) {
        scheme->uiPressed = layer(lightPlate ? 0.11 : 0.16);
    }
}

namespace {

TerminalColorScheme schemeFromHex(const char *name,
                                  const char *foreground,
                                  const char *background,
                                  const char *cursor,
                                  const char *selection,
                                  std::initializer_list<const char *> ansi) {
    TerminalColorScheme scheme;
    scheme.name = QLatin1String(name);
    scheme.foreground = QColor(QLatin1String(foreground));
    scheme.background = QColor(QLatin1String(background));
    scheme.cursorColor = QColor(QLatin1String(cursor));
    scheme.selectionBackground = QColor(QLatin1String(selection));
    scheme.ansi = paletteFromHex(ansi);
    completeSchemeUi(&scheme);
    return scheme;
}

TerminalColorScheme familyScheme(const char *name,
                                 bool dark,
                                 const char *uiBackground,
                                 const char *uiPlate,
                                 const char *uiAccent,
                                 const char *foreground,
                                 const char *background,
                                 const char *cursor,
                                 const char *selection) {
    TerminalColorScheme scheme;
    scheme.name = QLatin1String(name);
    scheme.foreground = QColor(QLatin1String(foreground));
    scheme.background = QColor(QLatin1String(background));
    scheme.cursorColor = QColor(QLatin1String(cursor));
    scheme.selectionBackground = QColor(QLatin1String(selection));
    scheme.ansi = defaultAnsiPalette(dark);
    scheme.uiBackground = QColor(QLatin1String(uiBackground));
    scheme.uiPlate = QColor(QLatin1String(uiPlate));
    scheme.uiAccent = QColor(QLatin1String(uiAccent));
    completeSchemeUi(&scheme);
    return scheme;
}

} // namespace

QVector<TerminalColorScheme> builtinColorSchemes() {
    QVector<TerminalColorScheme> schemes;
    schemes.reserve(19);
    // Span: warm stone neutrals with a soft denim accent and the rose cursor
    // as signature. Panels sit one visible step off the content paper;
    // list selection is a soft accent tint (hover and press stay neutral), so
    // the selected row reads apart from the hover pill.
    TerminalColorScheme spanDark = familyScheme(kDefaultDarkColorScheme.data(), true, "#131211", "#1b1a18", "#93a5d8",
                                                "#ebe7df", "#131211", "#d86575", "#3d93a5d8");
    spanDark.uiSelection = QColor(QLatin1String("#3d93a5d8"));
    schemes.append(spanDark);
    TerminalColorScheme spanLight = familyScheme(kDefaultLightColorScheme.data(), false, "#f5f3ef", "#fbfaf8",
                                                 "#4661b3", "#26231e", "#f5f3ef", "#d86575", "#2e4661b3");
    spanLight.uiSelection = QColor(QLatin1String("#244661b3"));
    schemes.append(spanLight);
    schemes.append(familyScheme(
        "Aurora Dark", true, "#12101c", "#161322", "#b8a6f5",
        "#ece8f8", "#12101c", "#c878e8", "#38b8a6f5"));
    schemes.append(familyScheme(
        "Aurora Light", false, "#f5f3fb", "#f7f6fc", "#5c4ec2",
        "#2c2842", "#f5f3fb", "#7a52d0", "#285c4ec2"));
    schemes.append(familyScheme(
        "Ember Dark", true, "#16110c", "#1c1610", "#e09a58",
        "#f3ebe3", "#16110c", "#e88850", "#38e09a58"));
    schemes.append(familyScheme(
        "Ember Light", false, "#fbf6ef", "#fdf9f3", "#b45a1c",
        "#3a2a1c", "#fbf6ef", "#c25a20", "#28b45a1c"));
    schemes.append(familyScheme(
        "Ocean Dark", true, "#0c121a", "#101820", "#6eb0e8",
        "#e6eef6", "#0c121a", "#4aa0e0", "#386eb0e8"));
    schemes.append(familyScheme(
        "Ocean Light", false, "#f2f6fa", "#f6f9fc", "#1e6aa8",
        "#1e2a36", "#f2f6fa", "#1f6ab8", "#281e6aa8"));
    schemes.append(familyScheme(
        "Forest Dark", true, "#0e1410", "#121a14", "#6ec89a",
        "#e8f2ea", "#0e1410", "#50c888", "#386ec89a"));
    schemes.append(familyScheme(
        "Forest Light", false, "#f3f8f3", "#f7faf7", "#1c7a48",
        "#1c2e22", "#f3f8f3", "#1e7a48", "#281c7a48"));
    schemes.append(familyScheme(
        "Dusk Dark", true, "#141018", "#18141e", "#c49ae6",
        "#f0e8f6", "#141018", "#d080e0", "#38c49ae6"));
    schemes.append(familyScheme(
        "Dusk Light", false, "#f6f2f8", "#f9f6fb", "#7a42ae",
        "#2e2438", "#f6f2f8", "#7a40b0", "#287a42ae"));
    // Windows Terminal classic schemes. Selection backgrounds that WT leaves
    // unset fall back to its opaque-white default.
    schemes.append(schemeFromHex(
        "Campbell", "#cccccc", "#0c0c0c", "#ffffff", "#ffffff", {
            "#0c0c0c", "#c50f1f", "#13a10e", "#c19c00",
            "#0037da", "#881798", "#3a96dd", "#cccccc",
            "#767676", "#e74856", "#16c60c", "#f9f1a5",
            "#3b78ff", "#b4009e", "#61d6d6", "#f2f2f2",
        }));
    schemes.append(schemeFromHex(
        "Campbell Powershell", "#cccccc", "#012456", "#ffffff", "#ffffff", {
            "#0c0c0c", "#c50f1f", "#13a10e", "#c19c00",
            "#0037da", "#881798", "#3a96dd", "#cccccc",
            "#767676", "#e74856", "#16c60c", "#f9f1a5",
            "#3b78ff", "#b4009e", "#61d6d6", "#f2f2f2",
        }));
    schemes.append(schemeFromHex(
        "Vintage", "#c0c0c0", "#000000", "#ffffff", "#ffffff", {
            "#000000", "#800000", "#008000", "#808000",
            "#000080", "#800080", "#008080", "#c0c0c0",
            "#808080", "#ff0000", "#00ff00", "#ffff00",
            "#0000ff", "#ff00ff", "#00ffff", "#ffffff",
        }));
    schemes.append(schemeFromHex(
        "One Half Dark", "#dcdfe4", "#282c34", "#ffffff", "#ffffff", {
            "#282c34", "#e06c75", "#98c379", "#e5c07b",
            "#61afef", "#c678dd", "#56b6c2", "#dcdfe4",
            "#5a6374", "#e06c75", "#98c379", "#e5c07b",
            "#61afef", "#c678dd", "#56b6c2", "#dcdfe4",
        }));
    schemes.append(schemeFromHex(
        "One Half Light", "#383a42", "#fafafa", "#4f525d", "#383a42", {
            "#383a42", "#e45649", "#50a14f", "#c18301",
            "#0184bc", "#a626a4", "#0997b3", "#fafafa",
            "#4f525d", "#df6c75", "#98c379", "#e4c07a",
            "#61afef", "#c577dd", "#56b5c1", "#ffffff",
        }));
    schemes.append(schemeFromHex(
        "Tango Dark", "#d3d7cf", "#000000", "#ffffff", "#ffffff", {
            "#000000", "#cc0000", "#4e9a06", "#c4a000",
            "#3465a4", "#75507b", "#06989a", "#d3d7cf",
            "#555753", "#ef2929", "#8ae234", "#fce94f",
            "#729fcf", "#ad7fa8", "#34e2e2", "#eeeeec",
        }));
    schemes.append(schemeFromHex(
        "Tango Light", "#555753", "#ffffff", "#000000", "#141414", {
            "#000000", "#cc0000", "#4e9a06", "#c4a000",
            "#3465a4", "#75507b", "#06989a", "#d3d7cf",
            "#555753", "#ef2929", "#8ae234", "#fce94f",
            "#729fcf", "#ad7fa8", "#34e2e2", "#eeeeec",
        }));
    return schemes;
}

QJsonObject schemeToJson(const TerminalColorScheme &scheme) {
    QJsonObject object;
    object.insert(QStringLiteral("foreground"), scheme.foreground.name(QColor::HexArgb));
    object.insert(QStringLiteral("background"), scheme.background.name(QColor::HexArgb));
    object.insert(QStringLiteral("cursor"), scheme.cursorColor.name(QColor::HexArgb));
    object.insert(QStringLiteral("selection"),
                  scheme.selectionBackground.name(QColor::HexArgb));
    object.insert(QStringLiteral("uiBackground"),
                  scheme.uiBackground.name(QColor::HexArgb));
    object.insert(QStringLiteral("uiPlate"), scheme.uiPlate.name(QColor::HexArgb));
    object.insert(QStringLiteral("uiAccent"), scheme.uiAccent.name(QColor::HexArgb));
    object.insert(QStringLiteral("uiHover"), scheme.uiHover.name(QColor::HexArgb));
    object.insert(QStringLiteral("uiSelection"),
                  scheme.uiSelection.name(QColor::HexArgb));
    object.insert(QStringLiteral("uiPressed"), scheme.uiPressed.name(QColor::HexArgb));
    QJsonArray ansi;
    for (const QColor &color : scheme.ansi) {
        ansi.append(color.name(QColor::HexArgb));
    }
    object.insert(QStringLiteral("ansi"), ansi);
    return object;
}

bool schemeFromJson(const QString &name,
                    const QJsonObject &object,
                    TerminalColorScheme *out) {
    TerminalColorScheme scheme;
    scheme.name = name;
    scheme.foreground = QColor(object.value(QStringLiteral("foreground")).toString());
    scheme.background = QColor(object.value(QStringLiteral("background")).toString());
    scheme.cursorColor = QColor(object.value(QStringLiteral("cursor")).toString());
    scheme.selectionBackground =
        QColor(object.value(QStringLiteral("selection")).toString());
    scheme.uiBackground = QColor(object.value(QStringLiteral("uiBackground")).toString());
    scheme.uiPlate = QColor(object.value(QStringLiteral("uiPlate")).toString());
    scheme.uiAccent = QColor(object.value(QStringLiteral("uiAccent")).toString());
    scheme.uiHover = QColor(object.value(QStringLiteral("uiHover")).toString());
    scheme.uiSelection = QColor(object.value(QStringLiteral("uiSelection")).toString());
    scheme.uiPressed = QColor(object.value(QStringLiteral("uiPressed")).toString());
    const QJsonArray ansi = object.value(QStringLiteral("ansi")).toArray();
    for (const QJsonValue &value : ansi) {
        scheme.ansi.append(QColor(value.toString()));
    }
    if (!scheme.isValid()) {
        return false;
    }
    completeSchemeUi(&scheme);
    *out = scheme;
    return true;
}

} // namespace span::terminal
