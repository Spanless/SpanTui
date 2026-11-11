#pragma once

#include <QPointF>
#include <QRectF>
#include <QString>
#include <QVector>
#include <QtGlobal>

// Glyphs drawn as geometry instead of font glyphs: box drawing, block
// elements and sextants, braille. They share one DPI-aware geometry so a
// theme, font-size or monitor change cannot make one family look thinner than
// the others.
namespace span::terminal {

class TerminalGlyphGeometry final {
public:
    static qreal devicePixelRatio(qreal ratio);
    static qreal snap(qreal value, qreal devicePixelRatio);
    static qreal lightStroke(qreal devicePixelRatio);
    static qreal heavyStroke(qreal devicePixelRatio);
    static qreal brailleDot(qreal devicePixelRatio);
};

struct TerminalCoverageMeshVertex {
    QPointF position;
    quint8 coverage = 0;
};

struct TerminalCoverageMesh {
    QVector<TerminalCoverageMeshVertex> vertices;
    QVector<quint16> indices;
    qreal fringeWidth = 0.0;

    bool isEmpty() const { return vertices.isEmpty() || indices.isEmpty(); }
};

class TerminalStrokeMeshBuilder final {
public:
    static TerminalCoverageMesh build(const QVector<QPointF> &centerLine, const QVector<QPointF> &normals,
                                      qreal strokeWidth, qreal devicePixelRatio);
};

// Box drawing (U+2500..U+257F) as stroked paths.
struct TerminalStrokePath {
    QVector<QPointF> centerLine;
    QVector<QPointF> normals;
    qreal width = 0.0;
};

struct TerminalBoxDrawingGeometry {
    QVector<TerminalStrokePath> strokes;

    bool isEmpty() const { return strokes.isEmpty(); }
};

class TerminalBoxDrawing final {
public:
    static bool supports(const QString &text);
    static TerminalBoxDrawingGeometry geometry(const QString &text, const QRectF &cell, qreal devicePixelRatio);
};

// Block elements (U+2580..U+259F) and sextants (U+1FB00..U+1FB3B) as
// pixel-aligned rectangles.
class TerminalBlockElements final {
public:
    static bool supports(const QString &text);
    static QVector<QRectF> rectangles(const QString &text, const QRectF &cell, qreal devicePixelRatio);
};

// Braille (U+2800..U+28FF) as dots on one terminal-wide lattice.
struct TerminalBrailleGeometry {
    QVector<QPointF> dotCenters;
    qreal coreDiameter = 0.0;
    qreal fringeWidth = 0.0;
    qreal horizontalPitch = 0.0;
    qreal verticalPitch = 0.0;

    bool isEmpty() const { return dotCenters.isEmpty() || coreDiameter <= 0.0; }
};

class TerminalBraille final {
public:
    static bool supports(const QString &text);
    static TerminalBrailleGeometry geometry(const QString &text, const QRectF &cell, qreal devicePixelRatio);
};

class TerminalBrailleMeshBuilder final {
public:
    static TerminalCoverageMesh build(const TerminalBrailleGeometry &geometry);
};

} // namespace span::terminal
