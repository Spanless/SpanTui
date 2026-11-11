#pragma once

#include "terminal/terminal_constants.h"

#include <QColor>
#include <QJsonArray>
#include <QJsonObject>
#include <QString>
#include <QVector>

namespace span::terminal {

qreal relativeLuminance(const QColor &color);
qreal contrastRatio(const QColor &first, const QColor &second);
bool isLightSurface(const QColor &background);

// Spawn-time COLORFGBG. Light surfaces report 0;15, dark or unknown 15;0.
QString colorFgBg(const QColor &foreground, const QColor &background);

QColor ensureMinimumContrast(const QColor &foreground,
                             const QColor &background,
                             qreal minimumRatio = kMinimumReadableContrast);

// Protocol surface colors. Must stay aligned with Theme.qml terminal
// defaults (text, terminalBackdrop, terminalAnsi).
QColor defaultSurfaceForeground(bool dark);
QColor defaultSurfaceBackground(bool dark);
QVector<QColor> defaultAnsiPalette(bool dark);

// A complete color scheme: terminal surfaces, the 16 ANSI slots, and the
// workspace paper that the UI reads. Names are user-visible persistence keys;
// built-in names are reserved and cannot be shadowed by user schemes.
struct TerminalColorScheme {
    QString name;
    QColor foreground;
    QColor background;
    QColor cursorColor;
    QColor selectionBackground;
    QVector<QColor> ansi; // indexes 0-15
    QColor uiBackground; // window / title-bar paper
    QColor uiPlate;      // card / material plate
    QColor uiAccent;     // chrome accent
    // Interaction state layers: translucent colors the UI composites over
    // whatever surface an element sits on (alpha = strength). Selected stacks
    // under hover/pressed; semantic variants reuse the alpha with their hue.
    QColor uiHover;
    QColor uiSelection;
    QColor uiPressed;

    bool isValid() const;
};

// Fill missing UI paper from the terminal background, and missing state
// layers from the foreground, so legacy JSON and the Windows Terminal classic
// schemes still load as a complete look.
void completeSchemeUi(TerminalColorScheme *scheme);

// Immutable factory set: Span Dark / Light plus family papers, then the
// Windows Terminal classic schemes (microsoft/terminal defaults.json, MIT).
QVector<TerminalColorScheme> builtinColorSchemes();

// Persistence form: surfaces, uiBackground/uiPlate/uiAccent,
// uiHover/uiSelection/uiPressed, ansi[16].
QJsonObject schemeToJson(const TerminalColorScheme &scheme);
bool schemeFromJson(const QString &name,
                    const QJsonObject &object,
                    TerminalColorScheme *out);

} // namespace span::terminal
