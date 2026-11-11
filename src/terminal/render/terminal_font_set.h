#pragma once

#include "terminal/api/terminal_types.h"

#include <QFont>

namespace span::terminal {

class TerminalFontSet final {
public:
    static QStringList availableFamilies(bool fixedPitchOnly);
    static QFont safeFixedFont(int pixelSize = 0);
    // Equal font whose private data is shared with no other QFont. QFont
    // caches its font engines in that data without locking, so a font used
    // on another thread must not share it with a font in use here.
    static QFont isolatedCopy(const QFont &font);
    // Cell text with every code point that no installed outline font maps
    // replaced by U+FFFD (an unmapped combining mark is dropped instead).
    // Qt looks for such a code point through every installed family, ending
    // in the raster ones DirectWrite cannot open, and logs a warning for each.
    static QString drawableText(const QString &text);

    void setFont(const QFont &font);
    void setDevicePixelRatio(qreal ratio);
    // Replaces the held fonts with isolated copies (see isolatedCopy).
    void isolate();

    QFont baseFont() const;
    QFont fontForStyle(const CellStyle &style) const;
    qreal cellWidth() const;
    qreal cellHeight() const;
    qreal baseline() const;
    qreal devicePixelRatio() const;
    bool boldFaceAvailable() const;

private:
    void updateMetrics();

    QFont m_requestedFont;
    QFont m_font;
    qreal m_devicePixelRatio = 1.0;
    qreal m_cellWidth = 8.0;
    qreal m_cellHeight = 18.0;
    qreal m_baseline = 14.0;
    QString m_effectivePrimary;
    bool m_primaryAvailable = false;
    bool m_fallbackActive = false;
    bool m_boldFaceAvailable = false;
    // Native face ANSI bold switches to; empty when there is none.
    QString m_boldStyleName;
    QFont::Weight m_boldWeight = QFont::Bold;
};

} // namespace span::terminal
