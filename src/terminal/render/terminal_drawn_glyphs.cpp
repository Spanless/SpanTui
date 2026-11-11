#include "terminal/render/terminal_drawn_glyphs.h"

#include <QtMath>

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <utility>

namespace span::terminal {

// ---- Shared geometry -------------------------------------------------------

qreal TerminalGlyphGeometry::devicePixelRatio(qreal ratio)
{
    return std::max<qreal>(1.0, ratio);
}

qreal TerminalGlyphGeometry::snap(qreal value, qreal devicePixelRatio)
{
    const qreal ratio = TerminalGlyphGeometry::devicePixelRatio(devicePixelRatio);
    return std::round(value * ratio) / ratio;
}

qreal TerminalGlyphGeometry::lightStroke(qreal devicePixelRatio)
{
    return 2.0 / TerminalGlyphGeometry::devicePixelRatio(devicePixelRatio);
}

qreal TerminalGlyphGeometry::heavyStroke(qreal devicePixelRatio)
{
    return 3.0 / TerminalGlyphGeometry::devicePixelRatio(devicePixelRatio);
}

qreal TerminalGlyphGeometry::brailleDot(qreal devicePixelRatio)
{
    return 2.0 / TerminalGlyphGeometry::devicePixelRatio(devicePixelRatio);
}

// ---- Stroke mesh -----------------------------------------------------------

namespace {

constexpr int kVerticesPerPoint = 4;
constexpr int kBandsPerSegment = 3;
constexpr int kIndicesPerBand = 6;
constexpr int kMaximumPointCount = std::numeric_limits<quint16>::max() / kVerticesPerPoint;

inline QPointF normalized(const QPointF &normal)
{
    const qreal lensq = normal.x() * normal.x() + normal.y() * normal.y();
    if (std::abs(lensq - 1.0) < 1e-5) {
        return normal;
    }
    if (lensq <= std::numeric_limits<qreal>::epsilon()) {
        return {};
    }
    return normal / std::sqrt(lensq);
}

} // namespace

TerminalCoverageMesh TerminalStrokeMeshBuilder::build(const QVector<QPointF> &centerLine,
                                                      const QVector<QPointF> &normals, qreal strokeWidth,
                                                      qreal devicePixelRatio)
{
    TerminalCoverageMesh result;
    if (centerLine.size() < 2 || centerLine.size() != normals.size() || centerLine.size() > kMaximumPointCount
        || strokeWidth <= 0.0) {
        return result;
    }

    const qreal ratio = TerminalGlyphGeometry::devicePixelRatio(devicePixelRatio);
    result.fringeWidth = 1.0 / ratio;
    const qreal halfWidth = strokeWidth / 2.0;
    const qreal halfFringe = result.fringeWidth / 2.0;
    const std::array<qreal, kVerticesPerPoint> offsets = {-halfWidth - halfFringe, -halfWidth + halfFringe,
                                                          halfWidth - halfFringe, halfWidth + halfFringe};
    const std::array<quint8, kVerticesPerPoint> coverage = {0, 255, 255, 0};

    result.vertices.reserve(centerLine.size() * kVerticesPerPoint);
    for (int pointIndex = 0; pointIndex < centerLine.size(); ++pointIndex) {
        const QPointF normal = normalized(normals.at(pointIndex));
        if (normal.isNull()) {
            return {};
        }
        for (int offsetIndex = 0; offsetIndex < kVerticesPerPoint; ++offsetIndex) {
            result.vertices.push_back(
                {centerLine.at(pointIndex) + normal * offsets.at(offsetIndex), coverage.at(offsetIndex)});
        }
    }

    result.indices.reserve((centerLine.size() - 1) * kBandsPerSegment * kIndicesPerBand);
    for (int segment = 0; segment < centerLine.size() - 1; ++segment) {
        const quint16 first = quint16(segment * kVerticesPerPoint);
        const quint16 next = quint16((segment + 1) * kVerticesPerPoint);
        for (quint16 band = 0; band < kBandsPerSegment; ++band) {
            result.indices.append(first + band);
            result.indices.append(next + band);
            result.indices.append(next + band + 1);
            result.indices.append(first + band);
            result.indices.append(next + band + 1);
            result.indices.append(first + band + 1);
        }
    }
    return result;
}

// ---- Box drawing -----------------------------------------------------------

namespace {

constexpr qreal kDuplicateTolerance = 0.000001;
// Rounded corners get enough segments to keep the chord error below this.
constexpr qreal kMaximumChordErrorInPhysicalPixels = 0.1;
constexpr int kMinimumQuarterCircleSegments = 4;

enum Direction : quint8 { Up = 0x1, Right = 0x2, Down = 0x4, Left = 0x8 };

struct Shape {
    quint8 directions = 0;
    bool heavy = false;
    int dashes = 0;
    enum class RoundedCorner : quint8 {
        None,
        TopLeft,
        TopRight,
        BottomRight,
        BottomLeft
    } roundedCorner = RoundedCorner::None;
};

bool inRange(char32_t codepoint, char32_t first, char32_t last)
{
    return codepoint >= first && codepoint <= last;
}

Shape shapeFor(char32_t codepoint)
{
    switch (codepoint) {
    case U'─': return {Left | Right};
    case U'━': return {Left | Right, true};
    case U'│': return {Up | Down};
    case U'┃': return {Up | Down, true};
    case U'┄': return {Left | Right, false, 3};
    case U'┅': return {Left | Right, true, 3};
    case U'┆': return {Up | Down, false, 3};
    case U'┇': return {Up | Down, true, 3};
    case U'┈': return {Left | Right, false, 4};
    case U'┉': return {Left | Right, true, 4};
    case U'┊': return {Up | Down, false, 4};
    case U'┋': return {Up | Down, true, 4};
    case U'╌': return {Left | Right, false, 2};
    case U'╍': return {Left | Right, true, 2};
    case U'╎': return {Up | Down, false, 2};
    case U'╏': return {Up | Down, true, 2};
    case U'╴': return {Left};
    case U'╵': return {Up};
    case U'╶': return {Right};
    case U'╷': return {Down};
    case U'╸': return {Left, true};
    case U'╹': return {Up, true};
    case U'╺': return {Right, true};
    case U'╻': return {Down, true};
    case U'╭': return {Right | Down, false, 0, Shape::RoundedCorner::TopLeft};
    case U'╮': return {Left | Down, false, 0, Shape::RoundedCorner::TopRight};
    case U'╯': return {Up | Left, false, 0, Shape::RoundedCorner::BottomRight};
    case U'╰': return {Up | Right, false, 0, Shape::RoundedCorner::BottomLeft};
    default: break;
    }

    if (inRange(codepoint, U'┌', U'┏')) return {Right | Down, codepoint == U'┏'};
    if (inRange(codepoint, U'┐', U'┓')) return {Left | Down, codepoint == U'┓'};
    if (inRange(codepoint, U'└', U'┗')) return {Up | Right, codepoint == U'┗'};
    if (inRange(codepoint, U'┘', U'┛')) return {Up | Left, codepoint == U'┛'};
    if (inRange(codepoint, U'├', U'┣')) return {Up | Right | Down, codepoint == U'┣'};
    if (inRange(codepoint, U'┤', U'┫')) return {Up | Left | Down, codepoint == U'┫'};
    if (inRange(codepoint, U'┬', U'┳')) return {Left | Right | Down, codepoint == U'┳'};
    if (inRange(codepoint, U'┴', U'┻')) return {Up | Left | Right, codepoint == U'┻'};
    if (inRange(codepoint, U'┼', U'╋')) return {Up | Right | Down | Left, codepoint == U'╋'};
    return {};
}

qreal alignedCenter(qreal center, qreal thickness, qreal ratio)
{
    return TerminalGlyphGeometry::snap(center - thickness / 2.0, ratio) + thickness / 2.0;
}

void appendPoint(TerminalStrokePath &path, const QPointF &point, const QPointF &normal)
{
    if (!path.centerLine.isEmpty()) {
        const QPointF delta = path.centerLine.constLast() - point;
        if (std::hypot(delta.x(), delta.y()) <= kDuplicateTolerance) {
            path.centerLine.last() = point;
            path.normals.last() = normal;
            return;
        }
    }
    path.centerLine.push_back(point);
    path.normals.push_back(normal);
}

void appendStraightStrokes(QVector<TerminalStrokePath> &result, qreal first, qreal last, qreal center, qreal thickness,
                           bool horizontal, qreal ratio, int dashes)
{
    const qreal start = std::min(first, last);
    const qreal end = std::max(first, last);
    const int count = std::max(1, dashes);
    const qreal alignedAxis = alignedCenter(center, thickness, ratio);
    for (int index = 0; index < count; ++index) {
        const qreal dashStart = start + (end - start) * index / count;
        const qreal dashEnd = dashes > 0 ? start + (end - start) * (index + 0.64) / count : end;
        const qreal alignedStart = TerminalGlyphGeometry::snap(dashStart, ratio);
        const qreal alignedEnd = TerminalGlyphGeometry::snap(dashEnd, ratio);
        if (alignedEnd <= alignedStart) {
            continue;
        }

        TerminalStrokePath path;
        path.width = thickness;
        if (horizontal) {
            path.centerLine = {QPointF(alignedStart, alignedAxis), QPointF(alignedEnd, alignedAxis)};
            path.normals = {QPointF(0, -1), QPointF(0, -1)};
        } else {
            path.centerLine = {QPointF(alignedAxis, alignedStart), QPointF(alignedAxis, alignedEnd)};
            path.normals = {QPointF(-1, 0), QPointF(-1, 0)};
        }
        result.push_back(std::move(path));
    }
}

int quarterCircleSegments(qreal radius, qreal ratio)
{
    const qreal physicalRadius = radius * ratio;
    if (physicalRadius <= kMaximumChordErrorInPhysicalPixels) {
        return kMinimumQuarterCircleSegments;
    }

    const qreal cosine = std::clamp(1.0 - kMaximumChordErrorInPhysicalPixels / physicalRadius, -1.0, 1.0);
    const qreal maximumAngle = 2.0 * std::acos(cosine);
    if (maximumAngle <= 0.0) {
        return kMinimumQuarterCircleSegments;
    }
    return std::max(kMinimumQuarterCircleSegments, int(std::ceil(M_PI_2 / maximumAngle)));
}

TerminalStrokePath roundedStroke(Shape::RoundedCorner corner, const QRectF &cell, qreal thickness, qreal ratio)
{
    const qreal centerX = alignedCenter(cell.center().x(), thickness, ratio);
    const qreal centerY = alignedCenter(cell.center().y(), thickness, ratio);
    const qreal left = TerminalGlyphGeometry::snap(cell.left(), ratio);
    const qreal right = TerminalGlyphGeometry::snap(cell.right(), ratio);
    const qreal top = TerminalGlyphGeometry::snap(cell.top(), ratio);
    const qreal bottom = TerminalGlyphGeometry::snap(cell.bottom(), ratio);
    const qreal radius = std::min({centerX - left, right - centerX, centerY - top, bottom - centerY});
    if (radius <= 0.0) {
        return {};
    }

    QPointF circleCenter;
    QPointF firstEndpoint;
    QPointF lastEndpoint;
    QPointF firstNormal;
    QPointF lastNormal;
    qreal startAngle = 0.0;
    qreal endAngle = 0.0;
    switch (corner) {
    case Shape::RoundedCorner::TopLeft:
        circleCenter = QPointF(centerX + radius, centerY + radius);
        firstEndpoint = QPointF(right, centerY);
        lastEndpoint = QPointF(centerX, bottom);
        firstNormal = QPointF(0, -1);
        lastNormal = QPointF(-1, 0);
        startAngle = -M_PI_2;
        endAngle = -M_PI;
        break;
    case Shape::RoundedCorner::TopRight:
        circleCenter = QPointF(centerX - radius, centerY + radius);
        firstEndpoint = QPointF(left, centerY);
        lastEndpoint = QPointF(centerX, bottom);
        firstNormal = QPointF(0, -1);
        lastNormal = QPointF(1, 0);
        startAngle = -M_PI_2;
        endAngle = 0.0;
        break;
    case Shape::RoundedCorner::BottomRight:
        circleCenter = QPointF(centerX - radius, centerY - radius);
        firstEndpoint = QPointF(left, centerY);
        lastEndpoint = QPointF(centerX, top);
        firstNormal = QPointF(0, 1);
        lastNormal = QPointF(1, 0);
        startAngle = M_PI_2;
        endAngle = 0.0;
        break;
    case Shape::RoundedCorner::BottomLeft:
        circleCenter = QPointF(centerX + radius, centerY - radius);
        firstEndpoint = QPointF(centerX, top);
        lastEndpoint = QPointF(right, centerY);
        firstNormal = QPointF(-1, 0);
        lastNormal = QPointF(0, 1);
        startAngle = M_PI;
        endAngle = M_PI_2;
        break;
    case Shape::RoundedCorner::None: return {};
    }

    const int segments = quarterCircleSegments(radius, ratio);

    TerminalStrokePath result;
    result.width = thickness;
    result.centerLine.reserve(segments + 3);
    result.normals.reserve(segments + 3);
    appendPoint(result, firstEndpoint, firstNormal);
    for (int index = 0; index <= segments; ++index) {
        const qreal progress = qreal(index) / segments;
        const qreal angle = startAngle + (endAngle - startAngle) * progress;
        QPointF point = circleCenter + QPointF(radius * std::cos(angle), radius * std::sin(angle));
        QPointF normal(std::cos(angle), std::sin(angle));
        if (index == 0) {
            normal = firstNormal;
        } else if (index == segments) {
            normal = lastNormal;
        }
        appendPoint(result, point, normal);
    }
    appendPoint(result, lastEndpoint, lastNormal);
    return result;
}

} // namespace

bool TerminalBoxDrawing::supports(const QString &text)
{
    if (text.size() != 1) {
        return false;
    }
    const char32_t codepoint = text.at(0).unicode();
    return shapeFor(codepoint).directions != 0;
}

TerminalBoxDrawingGeometry TerminalBoxDrawing::geometry(const QString &text, const QRectF &cell, qreal devicePixelRatio)
{
    if (!supports(text)) {
        return {};
    }

    const Shape shape = shapeFor(text.at(0).unicode());
    const qreal ratio = TerminalGlyphGeometry::devicePixelRatio(devicePixelRatio);
    const qreal thickness =
        shape.heavy ? TerminalGlyphGeometry::heavyStroke(ratio) : TerminalGlyphGeometry::lightStroke(ratio);
    const qreal centerX = alignedCenter(cell.center().x(), thickness, ratio);
    const qreal centerY = alignedCenter(cell.center().y(), thickness, ratio);
    TerminalBoxDrawingGeometry result;
    if (shape.roundedCorner != Shape::RoundedCorner::None) {
        TerminalStrokePath stroke = roundedStroke(shape.roundedCorner, cell, thickness, ratio);
        if (!stroke.centerLine.isEmpty()) {
            result.strokes.push_back(std::move(stroke));
        }
        return result;
    }

    if (shape.directions & (Left | Right)) {
        appendStraightStrokes(result.strokes, (shape.directions & Left) ? cell.left() : centerX,
                              (shape.directions & Right) ? cell.right() : centerX, centerY, thickness, true, ratio,
                              shape.dashes);
    }
    if (shape.directions & (Up | Down)) {
        appendStraightStrokes(result.strokes, (shape.directions & Up) ? cell.top() : centerY,
                              (shape.directions & Down) ? cell.bottom() : centerY, centerX, thickness, false, ratio,
                              shape.dashes);
    }
    return result;
}

// ---- Block elements and sextants -------------------------------------------

namespace {

// Shades light a share of a 4×8 sub-cell grid; the strides spread the lit
// sub-cells evenly.
constexpr int kShadeColumns = 4;
constexpr int kShadeRows = 8;
constexpr int kShadeCells = kShadeColumns * kShadeRows;
constexpr int kShadeRowStride = 5;
constexpr int kShadeColumnStride = 7;

QRectF alignedRect(const QRectF &cell, qreal leftFraction, qreal topFraction, qreal rightFraction, qreal bottomFraction,
                   qreal ratio)
{
    const qreal left = TerminalGlyphGeometry::snap(cell.left() + cell.width() * leftFraction, ratio);
    const qreal top = TerminalGlyphGeometry::snap(cell.top() + cell.height() * topFraction, ratio);
    const qreal right = TerminalGlyphGeometry::snap(cell.left() + cell.width() * rightFraction, ratio);
    const qreal bottom = TerminalGlyphGeometry::snap(cell.top() + cell.height() * bottomFraction, ratio);
    return QRectF(left, top, std::max<qreal>(0.0, right - left), std::max<qreal>(0.0, bottom - top));
}

QVector<QRectF> shadePattern(const QRectF &cell, int density, qreal ratio)
{
    QVector<QRectF> result;
    const int threshold = std::clamp(density, 0, kShadeCells);
    for (int row = 0; row < kShadeRows; ++row) {
        for (int column = 0; column < kShadeColumns; ++column) {
            const int value = (row * kShadeRowStride + column * kShadeColumnStride) % kShadeCells;
            if (value >= threshold) {
                continue;
            }
            result.push_back(alignedRect(cell, qreal(column) / kShadeColumns, qreal(row) / kShadeRows,
                                         qreal(column + 1) / kShadeColumns, qreal(row + 1) / kShadeRows, ratio));
        }
    }
    return result;
}

// The single code point a cell holds, or 0 for clusters and empty cells.
char32_t singleCodePoint(const QString &text)
{
    if (text.size() == 1 && !text.at(0).isSurrogate()) {
        return text.at(0).unicode();
    }
    if (text.size() == 2 && text.at(0).isHighSurrogate() && text.at(1).isLowSurrogate()) {
        return QChar::surrogateToUcs4(text.at(0), text.at(1));
    }
    return 0;
}

bool isBlockElement(char32_t codepoint)
{
    return codepoint >= 0x2580 && codepoint <= 0x259f;
}

// Sextants U+1FB00..U+1FB3B enumerate the 2x3 masks 1..62 in order (bit 0 is
// the top-left sixth, bit 5 the bottom-right), skipping 21 and 42: the left
// and right halves already exist as U+258C and U+2590.
bool isSextant(char32_t codepoint)
{
    return codepoint >= 0x1fb00 && codepoint <= 0x1fb3b;
}

QVector<QRectF> sextantRectangles(char32_t codepoint, const QRectF &cell, qreal ratio)
{
    int mask = int(codepoint - 0x1fb00) + 1;
    if (mask >= 21) {
        ++mask;
    }
    if (mask >= 42) {
        ++mask;
    }
    QVector<QRectF> result;
    for (int row = 0; row < 3; ++row) {
        const bool left = mask & (1 << (row * 2));
        const bool right = mask & (1 << (row * 2 + 1));
        if (!left && !right) {
            continue;
        }
        // A full row is one rectangle so no seam shows at the middle column.
        result.push_back(
            alignedRect(cell, left ? 0.0 : 0.5, qreal(row) / 3.0, right ? 1.0 : 0.5, qreal(row + 1) / 3.0, ratio));
    }
    return result;
}

} // namespace

bool TerminalBlockElements::supports(const QString &text)
{
    const char32_t codepoint = singleCodePoint(text);
    return isBlockElement(codepoint) || isSextant(codepoint);
}

QVector<QRectF> TerminalBlockElements::rectangles(const QString &text, const QRectF &cell, qreal devicePixelRatio)
{
    if (!supports(text)) {
        return {};
    }

    const qreal ratio = TerminalGlyphGeometry::devicePixelRatio(devicePixelRatio);
    const char32_t codepoint = singleCodePoint(text);
    if (isSextant(codepoint)) {
        return sextantRectangles(codepoint, cell, ratio);
    }
    const auto rect = [&](qreal left, qreal top, qreal right, qreal bottom) {
        return QVector<QRectF>{alignedRect(cell, left, top, right, bottom, ratio)};
    };
    const auto lower = [&](qreal fraction) { return rect(0, 1.0 - fraction, 1, 1); };
    const auto left = [&](qreal fraction) { return rect(0, 0, fraction, 1); };
    const auto right = [&](qreal fraction) { return rect(1.0 - fraction, 0, 1, 1); };

    switch (codepoint) {
    case 0x2580: return rect(0, 0, 1, 0.5);
    case 0x2581: return lower(1.0 / 8.0);
    case 0x2582: return lower(2.0 / 8.0);
    case 0x2583: return lower(3.0 / 8.0);
    case 0x2584: return lower(0.5);
    case 0x2585: return lower(5.0 / 8.0);
    case 0x2586: return lower(6.0 / 8.0);
    case 0x2587: return lower(7.0 / 8.0);
    case 0x2588: return rect(0, 0, 1, 1);
    case 0x2589: return left(7.0 / 8.0);
    case 0x258a: return left(6.0 / 8.0);
    case 0x258b: return left(5.0 / 8.0);
    case 0x258c: return left(0.5);
    case 0x258d: return left(3.0 / 8.0);
    case 0x258e: return left(2.0 / 8.0);
    case 0x258f: return left(1.0 / 8.0);
    case 0x2590: return right(0.5);
    case 0x2591: return shadePattern(cell, kShadeCells / 4, ratio);
    case 0x2592: return shadePattern(cell, kShadeCells / 2, ratio);
    case 0x2593: return shadePattern(cell, kShadeCells * 3 / 4, ratio);
    case 0x2594: return rect(0, 0, 1, 1.0 / 8.0);
    case 0x2595: return right(1.0 / 8.0);
    case 0x2596: return rect(0, 0.5, 0.5, 1);
    case 0x2597: return rect(0.5, 0.5, 1, 1);
    case 0x2598: return rect(0, 0, 0.5, 0.5);
    case 0x2599: return {alignedRect(cell, 0, 0, 0.5, 1, ratio), alignedRect(cell, 0.5, 0.5, 1, 1, ratio)};
    case 0x259a: return {alignedRect(cell, 0, 0, 0.5, 0.5, ratio), alignedRect(cell, 0.5, 0.5, 1, 1, ratio)};
    case 0x259b: return {alignedRect(cell, 0, 0, 1, 0.5, ratio), alignedRect(cell, 0, 0.5, 0.5, 1, ratio)};
    case 0x259c: return {alignedRect(cell, 0, 0, 1, 0.5, ratio), alignedRect(cell, 0.5, 0.5, 1, 1, ratio)};
    case 0x259d: return rect(0.5, 0, 1, 0.5);
    case 0x259e: return {alignedRect(cell, 0.5, 0, 1, 1, ratio), alignedRect(cell, 0, 0.5, 0.5, 1, ratio)};
    case 0x259f: return {alignedRect(cell, 0.5, 0, 1, 1, ratio), alignedRect(cell, 0, 0.5, 0.5, 1, ratio)};
    default: return {};
    }
}

// ---- Braille ---------------------------------------------------------------

namespace {

constexpr char16_t kBrailleFirst = 0x2800;
constexpr char16_t kBrailleLast = 0x28ff;
constexpr int kBrailleDotCount = 8;
// Unicode dot order: 1,2,3,7 on the left; 4,5,6,8 on the right.
constexpr std::array<int, kBrailleDotCount> kDotColumns = {0, 0, 0, 1, 1, 1, 0, 1};
constexpr std::array<int, kBrailleDotCount> kDotRows = {0, 1, 2, 0, 1, 2, 3, 3};
constexpr int kCircleSegments = 16;
constexpr int kVerticesWithoutFringe = 1 + kCircleSegments;
constexpr int kVerticesWithFringe = 1 + kCircleSegments * 2;
constexpr int kFanIndices = kCircleSegments * 3;
constexpr int kFringeIndices = kCircleSegments * 6;
constexpr qreal kMaximumFringePixels = 0.5;
constexpr qreal kTwoPi = 2.0 * M_PI;

} // namespace

bool TerminalBraille::supports(const QString &text)
{
    return text.size() == 1 && text.at(0).unicode() >= kBrailleFirst && text.at(0).unicode() <= kBrailleLast;
}

TerminalBrailleGeometry TerminalBraille::geometry(const QString &text, const QRectF &cell, qreal devicePixelRatio)
{
    TerminalBrailleGeometry result;
    if (!supports(text) || cell.width() <= 0.0 || cell.height() <= 0.0) {
        return result;
    }

    const qreal ratio = TerminalGlyphGeometry::devicePixelRatio(devicePixelRatio);
    const qreal physicalLeft = std::round(cell.left() * ratio);
    const qreal physicalTop = std::round(cell.top() * ratio);
    const qreal physicalRight = std::round(cell.right() * ratio);
    const qreal physicalBottom = std::round(cell.bottom() * ratio);
    const qreal physicalWidth = physicalRight - physicalLeft;
    const qreal physicalHeight = physicalBottom - physicalTop;
    const qreal coreDiameterPixels = TerminalGlyphGeometry::brailleDot(ratio) * ratio;
    if (physicalWidth < coreDiameterPixels * 2.0 || physicalHeight < coreDiameterPixels * 4.0) {
        return result;
    }

    // A Braille cell is exactly two columns by four rows of one terminal-wide
    // lattice. Preserve fractional physical-pixel phases when a snapped cell
    // size is not divisible by two or four: rounding each glyph's origin or
    // moving the remainder into its margins creates a larger gap at cell
    // boundaries and bends diagonals that cross terminal rows.
    const qreal horizontalPitchPixels = physicalWidth / 2.0;
    const qreal verticalPitchPixels = physicalHeight / 4.0;
    const qreal firstCenterX = physicalLeft + horizontalPitchPixels / 2.0;
    const qreal firstCenterY = physicalTop + verticalPitchPixels / 2.0;
    const std::array<qreal, 2> xCenters = {firstCenterX, firstCenterX + horizontalPitchPixels};
    const std::array<qreal, 4> yCenters = {firstCenterY, firstCenterY + verticalPitchPixels,
                                           firstCenterY + verticalPitchPixels * 2.0,
                                           firstCenterY + verticalPitchPixels * 3.0};

    const qreal coreRadiusPixels = coreDiameterPixels / 2.0;
    const qreal availableFringePixels = std::max<qreal>(
        0.0,
        std::min({kMaximumFringePixels, xCenters.front() - coreRadiusPixels - physicalLeft,
                  physicalRight - xCenters.back() - coreRadiusPixels, yCenters.front() - coreRadiusPixels - physicalTop,
                  physicalBottom - yCenters.back() - coreRadiusPixels}));

    result.coreDiameter = coreDiameterPixels / ratio;
    result.fringeWidth = availableFringePixels / ratio;
    result.horizontalPitch = horizontalPitchPixels / ratio;
    result.verticalPitch = verticalPitchPixels / ratio;

    const quint8 mask = quint8(text.at(0).unicode() - kBrailleFirst);
    result.dotCenters.reserve(kBrailleDotCount);
    for (int dot = 0; dot < kBrailleDotCount; ++dot) {
        if (mask & (1u << dot)) {
            result.dotCenters.push_back(
                {xCenters.at(kDotColumns.at(dot)) / ratio, yCenters.at(kDotRows.at(dot)) / ratio});
        }
    }
    return result;
}

TerminalCoverageMesh TerminalBrailleMeshBuilder::build(const TerminalBrailleGeometry &geometry)
{
    TerminalCoverageMesh result;
    if (geometry.isEmpty()) {
        return result;
    }

    const bool hasFringe = geometry.fringeWidth > 0.0;
    const int verticesPerDot = hasFringe ? kVerticesWithFringe : kVerticesWithoutFringe;
    const int indicesPerDot = kFanIndices + (hasFringe ? kFringeIndices : 0);
    if (geometry.dotCenters.size() > std::numeric_limits<quint16>::max() / verticesPerDot) {
        return result;
    }

    result.fringeWidth = geometry.fringeWidth;
    result.vertices.reserve(geometry.dotCenters.size() * verticesPerDot);
    result.indices.reserve(geometry.dotCenters.size() * indicesPerDot);
    const qreal coreRadius = geometry.coreDiameter / 2.0;
    const qreal outerRadius = coreRadius + geometry.fringeWidth;

    for (const QPointF &center : geometry.dotCenters) {
        const quint16 base = quint16(result.vertices.size());
        result.vertices.push_back({center, 255});
        for (int segment = 0; segment < kCircleSegments; ++segment) {
            const qreal angle = kTwoPi * qreal(segment) / qreal(kCircleSegments);
            const QPointF direction(std::cos(angle), std::sin(angle));
            result.vertices.push_back({center + direction * coreRadius, 255});
        }
        if (hasFringe) {
            for (int segment = 0; segment < kCircleSegments; ++segment) {
                const qreal angle = kTwoPi * qreal(segment) / qreal(kCircleSegments);
                const QPointF direction(std::cos(angle), std::sin(angle));
                result.vertices.push_back({center + direction * outerRadius, 0});
            }
        }

        const quint16 inner = base + 1;
        const quint16 outer = inner + kCircleSegments;
        for (quint16 segment = 0; segment < kCircleSegments; ++segment) {
            const quint16 next = quint16((segment + 1) % kCircleSegments);
            result.indices.append(base);
            result.indices.append(inner + segment);
            result.indices.append(inner + next);
            if (hasFringe) {
                result.indices.append(inner + segment);
                result.indices.append(outer + segment);
                result.indices.append(outer + next);
                result.indices.append(inner + segment);
                result.indices.append(outer + next);
                result.indices.append(inner + next);
            }
        }
    }
    return result;
}

} // namespace span::terminal
