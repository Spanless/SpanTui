#include "terminal/render/terminal_row_layout.h"

#include "terminal/core/terminal_color.h"
#include "terminal/render/terminal_drawn_glyphs.h"

#include <algorithm>
#include <cmath>

namespace span::terminal {

namespace {

QColor resolveColor(const QColor &explicitColor,
                    bool isDefault,
                    int paletteIndex,
                    const QColor &defaultColor,
                    const QVector<QColor> &ansiPalette) {
    if (isDefault) {
        return defaultColor;
    }
    if (paletteIndex >= 0 && paletteIndex < ansiPalette.size()) {
        return ansiPalette.at(paletteIndex);
    }
    return explicitColor.isValid() ? explicitColor : defaultColor;
}

// SGR 2 halves the ink's distance to the cell background, as xterm and
// Windows Terminal do, so faint text stays faint on any theme.
QColor faintColor(const QColor &ink, const QColor &background) {
    if (!background.isValid() || background.alpha() == 0) {
        QColor result = ink;
        result.setAlphaF(ink.alphaF() * 0.5);
        return result;
    }
    const auto mix = [](int from, int to) { return (from + to + 1) / 2; };
    return QColor(mix(ink.red(), background.red()), mix(ink.green(), background.green()),
                  mix(ink.blue(), background.blue()), ink.alpha());
}

} // namespace

TerminalRowLayout TerminalRowLayoutBuilder::build(const Line &line,
                                                   const TerminalFontSet &fonts,
                                                   const QColor &foreground,
                                                   const QColor &background,
                                                   const QVector<QColor> &ansiPalette,
                                                   bool blinkVisible) {
    TerminalRowLayout result;
    const qreal cellWidth = fonts.cellWidth();
    const qreal cellHeight = fonts.cellHeight();
    const qreal ratio = fonts.devicePixelRatio();
    TerminalBackgroundRun currentBackground;
    bool hasBackground = false;
    // fontForStyle() builds a new QFont each call; cells of one style share
    // one, which also lets the glyph cache match fonts by their shared data.
    QFont styleFonts[8];
    bool styleFontReady[8] = {};
    const auto fontFor = [&](const CellStyle &style) -> const QFont & {
        const int key = (style.bold ? 1 : 0) | (style.italic ? 2 : 0) | (style.strike ? 4 : 0);
        if (!styleFontReady[key]) {
            styleFonts[key] = fonts.fontForStyle(style);
            styleFontReady[key] = true;
        }
        return styleFonts[key];
    };

    const auto flushBackground = [&]() {
        if (hasBackground) {
            result.backgroundRuns.push_back(std::move(currentBackground));
            currentBackground = {};
            hasBackground = false;
        }
    };

    for (int column = 0; column < line.cells.size(); ++column) {
        const Cell &cell = line.cells.at(column);
        if (cell.continuation) {
            continue;
        }

        const int span = std::max(1, cell.width);
        const qreal x = TerminalGlyphGeometry::snap(column * cellWidth, ratio);
        const qreal spanWidth = TerminalGlyphGeometry::snap(span * cellWidth, ratio);
        QColor cellForeground = resolveColor(cell.style.foreground,
                                             cell.style.defaultForeground,
                                             cell.style.foregroundPaletteIndex,
                                             foreground,
                                             ansiPalette);
        QColor cellBackground = resolveColor(cell.style.background,
                                             cell.style.defaultBackground,
                                             cell.style.backgroundPaletteIndex,
                                             background,
                                             ansiPalette);
        if (cell.style.reverse) {
            std::swap(cellForeground, cellBackground);
        }
        const bool drawBackground = cellBackground.isValid() && cellBackground.alpha() > 0 &&
                                    cellBackground != background;
        if (drawBackground && hasBackground && currentBackground.color == cellBackground &&
            qFuzzyCompare(currentBackground.rect.right(), x)) {
            currentBackground.rect.setWidth(currentBackground.rect.width() + spanWidth);
        } else {
            flushBackground();
            if (drawBackground) {
                currentBackground.rect = QRectF(x, 0, spanWidth, cellHeight);
                currentBackground.color = cellBackground;
                hasBackground = true;
            }
        }

        // Concealed cells keep their background only: no glyph, no
        // decoration, and nothing for contrast repair to re-ink.
        if (cell.text.isEmpty() || cell.style.conceal) {
            continue;
        }

        const QFont &font = fontFor(cell.style);
        const bool customChrome = TerminalBoxDrawing::supports(cell.text)
            || TerminalBlockElements::supports(cell.text)
            || TerminalBraille::supports(cell.text);
        // Contrast is against the resolved cell pair, not the theme slot.
        // Light-theme ANSI/default ink on a dark Codex composer, or dark-theme
        // gray on the light plane, both fail the same way. Chrome glyphs stay
        // put so dim tracks are not turned into a solid bar.
        // Faint applies after contrast repair, which would otherwise undo it.
        QColor paintColor = customChrome
            ? cellForeground
            : ensureMinimumContrast(cellForeground, cellBackground);
        if (cell.style.faint) {
            paintColor = faintColor(paintColor, cellBackground);
        }

        if (cell.style.underline != UnderlineStyle::None || cell.style.strike
            || cell.style.overline) {
            const bool followsText = cell.style.underlineColorPaletteIndex < 0
                && !cell.style.underlineColor.isValid();
            const QColor underlineColor = followsText
                ? paintColor
                : resolveColor(cell.style.underlineColor, false,
                               cell.style.underlineColorPaletteIndex, paintColor, ansiPalette);
            result.decorations.push_back({QRectF(x, 0, spanWidth, cellHeight), paintColor,
                                          underlineColor, cell.style.underline,
                                          cell.style.strike, cell.style.overline});
        }
        if (customChrome) {
            result.customGlyphs.push_back({cell.text,
                                           QRectF(x, 0, spanWidth, cellHeight),
                                           cell.style.blink && !blinkVisible ? Qt::transparent : paintColor});
            continue;
        }

        result.glyphCells.push_back({TerminalFontSet::drawableText(cell.text), font,
                                     cell.style.blink && !blinkVisible ? Qt::transparent : paintColor,
                                     QRectF(x, 0, spanWidth, cellHeight), column, span});
    }

    flushBackground();
    return result;
}

} // namespace span::terminal
