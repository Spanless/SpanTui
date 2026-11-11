#include "terminal/render/terminal_font_set.h"

#include <QFontDatabase>
#include <QFontInfo>
#include <QFontMetricsF>
#include <QHash>
#include <QMutex>
#include <QRawFont>

#include <algorithm>
#include <cmath>
#include <optional>
#include <utility>
#include <vector>

namespace span::terminal {

namespace {
constexpr auto kBundledTerminalFamily = "Span Mono";
// Nerd icons and Powerline on Span Mono's cell; a fallback only, never a pick.
constexpr auto kBundledSymbolsFamily = "Span Symbols";

qreal snapUp(qreal value, qreal ratio) {
    return std::ceil(value * ratio) / ratio;
}

bool hasFamily(const QStringList &families, const QString &family) {
    return std::any_of(families.cbegin(), families.cend(), [&family](const QString &candidate) {
        return candidate.compare(family, Qt::CaseInsensitive) == 0;
    });
}

QString matchingFamily(const QStringList &families, const QString &family) {
    const auto match = std::find_if(families.cbegin(), families.cend(), [&family](const QString &candidate) {
        return candidate.compare(family, Qt::CaseInsensitive) == 0;
    });
    return match == families.cend() ? QString() : *match;
}

bool isLegacyBitmapFamily(const QString &family) {
    static const QStringList legacyFamilies {
        QStringLiteral("MS Sans Serif"),
        QStringLiteral("MS Serif"),
        QStringLiteral("Fixedsys"),
        QStringLiteral("Small Fonts"),
        QStringLiteral("System"),
        // Windows LOGFONT logical bitmap families. DirectWrite cannot
        // instantiate these aliases, so probing isFixedPitch() emits one
        // warning per advertised raster size.
        QStringLiteral("Roman"),
        QStringLiteral("Swiss"),
        QStringLiteral("Modern"),
        QStringLiteral("Script"),
        QStringLiteral("Decorative"),
        QStringLiteral("Terminal")
    };
    return hasFamily(legacyFamilies, family);
}

// Code points Qt never looks up in a font: joiners, bidi and other format
// controls, variation selectors.
bool isIgnorableCodePoint(uint codePoint)
{
    return QChar::category(codePoint) == QChar::Other_Format || (codePoint >= 0xFE00 && codePoint <= 0xFE0F)
        || (codePoint >= 0xE0100 && codePoint <= 0xE01EF);
}

bool isMarkCodePoint(uint codePoint)
{
    const QChar::Category category = QChar::category(codePoint);
    return category == QChar::Mark_NonSpacing || category == QChar::Mark_SpacingCombining
        || category == QChar::Mark_Enclosing;
}

// Whether any installed outline family maps the code point. Families load
// once, on the first code point outside ASCII; answers are kept for the run.
bool outlineFontHasGlyph(uint codePoint)
{
    static QMutex mutex;
    static QHash<uint, bool> known;
    static QStringList families;
    static std::vector<std::optional<QRawFont>> fonts;
    static bool familiesListed = false;

    QMutexLocker locker(&mutex);
    const auto cached = known.constFind(codePoint);
    if (cached != known.cend()) {
        return *cached;
    }
    if (!familiesListed) {
        for (const QString &family : QFontDatabase::families()) {
            if (!isLegacyBitmapFamily(family) && QFontDatabase::isSmoothlyScalable(family)) {
                families.push_back(family);
            }
        }
        fonts.resize(size_t(families.size()));
        familiesListed = true;
    }
    bool found = false;
    for (qsizetype index = 0; index < families.size() && !found; ++index) {
        std::optional<QRawFont> &font = fonts[size_t(index)];
        if (!font) {
            QFont request(families.at(index));
            request.setStyleStrategy(QFont::NoFontMerging);
            request.setPixelSize(16);
            font = QRawFont::fromFont(request);
        }
        found = font->isValid() && font->supportsCharacter(codePoint);
    }
    known.insert(codePoint, found);
    return found;
}

bool hasRealBoldFace(const QString &family) {
    if (family.isEmpty()) {
        return false;
    }
    const QStringList styles = QFontDatabase::styles(family);
    return std::any_of(styles.cbegin(), styles.cend(), [&family](const QString &style) {
        const QFont face = QFontDatabase::font(family, style, 14);
        return face.weight() >= QFont::Bold;
    });
}

struct NativeFontFace {
    QString style;
    QFont::Weight weight = QFont::Normal;
};

NativeFontFace nearestNativeFace(const QString &family, QFont::Weight requested,
                                 bool requireBold = false) {
    QList<NativeFontFace> faces;
    for (const QString &style : QFontDatabase::styles(family)) {
        const QFont face = QFontDatabase::font(family, style, 14);
        const QFont::Weight weight = face.weight();
        if (!face.italic() && (!requireBold || weight >= QFont::Bold)) {
            const bool duplicate = std::any_of(faces.cbegin(), faces.cend(), [weight](const NativeFontFace &item) {
                return item.weight == weight;
            });
            if (!duplicate) {
                faces.push_back({style, weight});
            }
        }
    }
    if (faces.isEmpty()) {
        return {};
    }
    NativeFontFace nearest = faces.constFirst();
    int distance = qAbs(int(requested) - int(nearest.weight));
    for (const NativeFontFace &face : faces) {
        const int candidateDistance = qAbs(int(requested) - int(face.weight));
        if (candidateDistance < distance) {
            nearest = face;
            distance = candidateDistance;
        }
    }
    return nearest;
}

struct FixedPitchResolution {
    QFont font;
    QString effectivePrimary;
    bool primaryAvailable = false;
    bool fallbackActive = false;
    bool boldFaceAvailable = false;
};

FixedPitchResolution resolvedFixedPitchFont(const QFont &requested) {
    const QStringList installedFamilies = QFontDatabase::families();
    QStringList requestedFamilies;
    for (const QString &family : requested.families()) {
        const QString trimmed = family.trimmed();
        if (!trimmed.isEmpty() && !hasFamily(requestedFamilies, trimmed)) {
            requestedFamilies.push_back(trimmed);
        }
    }

    const QString requestedPrimary = requestedFamilies.value(0);
    const QString installedPrimary = matchingFamily(installedFamilies, requestedPrimary);
    const bool primaryAvailable = !installedPrimary.isEmpty();
    QStringList installedFallbacks;
    for (int index = 1; index < requestedFamilies.size(); ++index) {
        const QString installed = matchingFamily(installedFamilies, requestedFamilies.at(index));
        if (!installed.isEmpty() && !hasFamily(installedFallbacks, installed)) {
            installedFallbacks.push_back(installed);
        }
    }

    QString effectivePrimary = primaryAvailable ? installedPrimary : QString();
    if (effectivePrimary.isEmpty()) {
        const auto fixedFallback = std::find_if(
            installedFallbacks.cbegin(), installedFallbacks.cend(), [](const QString &family) {
                return QFontDatabase::isFixedPitch(family);
            });
        if (fixedFallback != installedFallbacks.cend()) {
            effectivePrimary = *fixedFallback;
        }
    }

    const QString bundledFamily =
        matchingFamily(installedFamilies, QString::fromLatin1(kBundledTerminalFamily));
    if (effectivePrimary.isEmpty() && !bundledFamily.isEmpty()
        && QFontDatabase::isFixedPitch(bundledFamily)) {
        effectivePrimary = bundledFamily;
    }
    if (effectivePrimary.isEmpty()) {
        effectivePrimary = TerminalFontSet::safeFixedFont().family();
    }

    QStringList resolvedFamilies;
    const auto appendResolved = [&resolvedFamilies](const QString &family) {
        if (!family.isEmpty() && !hasFamily(resolvedFamilies, family)) {
            resolvedFamilies.push_back(family);
        }
    };
    appendResolved(effectivePrimary);
    for (const QString &fallback : installedFallbacks) {
        appendResolved(fallback);
    }
    appendResolved(bundledFamily);
    appendResolved(matchingFamily(installedFamilies, QString::fromLatin1(kBundledSymbolsFamily)));
    appendResolved(matchingFamily(installedFamilies, QStringLiteral("Segoe UI Emoji")));
    appendResolved(matchingFamily(installedFamilies, QStringLiteral("Microsoft YaHei UI")));

    const NativeFontFace nativeFace = nearestNativeFace(effectivePrimary, requested.weight());
    QFont resolved = requested;
    if (resolvedFamilies.isEmpty()) {
        resolved = TerminalFontSet::safeFixedFont();
        resolvedFamilies = resolved.families();
    } else {
        if (!nativeFace.style.isEmpty()) {
            resolved = QFontDatabase::font(effectivePrimary, nativeFace.style, 14);
            resolved.setStyleName(nativeFace.style);
            resolved.setWeight(nativeFace.weight);
        }
        resolved.setFamilies(resolvedFamilies);
    }
    if (requested.pixelSize() > 0) {
        resolved.setPixelSize(requested.pixelSize());
    } else if (requested.pointSizeF() > 0) {
        resolved.setPointSizeF(requested.pointSizeF());
    }
    resolved.setStyle(requested.style());

    FixedPitchResolution result;
    result.font = std::move(resolved);
    result.effectivePrimary = effectivePrimary;
    result.primaryAvailable = primaryAvailable;
    result.fallbackActive = !requestedPrimary.isEmpty()
                            && effectivePrimary.compare(requestedPrimary,
                                                        Qt::CaseInsensitive) != 0;
    result.boldFaceAvailable = hasRealBoldFace(effectivePrimary);
    return result;
}
}

QStringList TerminalFontSet::availableFamilies(bool fixedPitchOnly) {
    QStringList families = QFontDatabase::families();
    families.erase(std::remove_if(families.begin(), families.end(), [](const QString &family) {
                       return isLegacyBitmapFamily(family)
                              || family.compare(QLatin1String(kBundledSymbolsFamily),
                                                Qt::CaseInsensitive) == 0;
                   }),
                   families.end());
    if (fixedPitchOnly) {
        families.erase(std::remove_if(families.begin(), families.end(), [](const QString &family) {
                           return !QFontDatabase::isFixedPitch(family);
                       }),
                       families.end());
    }
    std::sort(families.begin(), families.end(), [](const QString &left, const QString &right) {
        return left.compare(right, Qt::CaseInsensitive) < 0;
    });
    return families;
}

QFont TerminalFontSet::safeFixedFont(int pixelSize) {
    const QStringList installedFamilies = QFontDatabase::families();
    const QStringList preferredFamilies {
        QString::fromLatin1(kBundledTerminalFamily),
        QStringLiteral("Cascadia Mono"),
        QStringLiteral("Cascadia Code"),
        QStringLiteral("JetBrains Mono"),
        QStringLiteral("Consolas"),
        QStringLiteral("Courier New")
    };

    QString family;
    for (const QString &preferred : preferredFamilies) {
        const QString matched = matchingFamily(installedFamilies, preferred);
        if (!matched.isEmpty() && QFontDatabase::isFixedPitch(matched) && !isLegacyBitmapFamily(matched)) {
            family = matched;
            break;
        }
    }
    if (family.isEmpty()) {
        for (const QString &candidate : installedFamilies) {
            if (QFontDatabase::isFixedPitch(candidate) && !isLegacyBitmapFamily(candidate)) {
                family = candidate;
                break;
            }
        }
    }

    QFont result;
    if (!family.isEmpty()) {
        result.setFamily(family);
    }
    result.setStyleHint(QFont::Monospace);
    result.setFixedPitch(true);
    if (pixelSize > 0) {
        result.setPixelSize(pixelSize);
    }
    return result;
}

QString TerminalFontSet::drawableText(const QString &text)
{
    const bool ascii =
        std::all_of(text.cbegin(), text.cend(), [](QChar character) { return character.unicode() < 0x80; });
    if (ascii) {
        return text;
    }
    QString result;
    result.reserve(text.size());
    bool changed = false;
    bool hasBase = false;
    for (const uint codePoint : text.toUcs4()) {
        const bool mark = isMarkCodePoint(codePoint);
        if (codePoint < 0x80 || isIgnorableCodePoint(codePoint) || outlineFontHasGlyph(codePoint)) {
            result.append(QStringView(QChar::fromUcs4(char32_t(codePoint))));
            hasBase = hasBase || !mark;
            continue;
        }
        changed = true;
        if (mark && hasBase) {
            continue;
        }
        result.append(QChar(0xFFFD));
        hasBase = true;
    }
    return changed ? result : text;
}

void TerminalFontSet::setFont(const QFont &font) {
    if (m_requestedFont == font) {
        return;
    }
    m_requestedFont = font;
    FixedPitchResolution resolution = resolvedFixedPitchFont(font);
    QFont resolved = std::move(resolution.font);
    resolved.setStyleHint(QFont::Monospace);
    resolved.setFixedPitch(true);
    resolved.setKerning(false);
    resolved.setHintingPreference(QFont::PreferVerticalHinting);
    resolved.setFeature(QFont::Tag("liga"), 0);
    resolved.setFeature(QFont::Tag("clig"), 0);
    if (m_font == resolved
        && m_effectivePrimary == resolution.effectivePrimary
        && m_primaryAvailable == resolution.primaryAvailable
        && m_fallbackActive == resolution.fallbackActive
        && m_boldFaceAvailable == resolution.boldFaceAvailable) {
        return;
    }
    m_font = std::move(resolved);
    m_effectivePrimary = std::move(resolution.effectivePrimary);
    m_primaryAvailable = resolution.primaryAvailable;
    m_fallbackActive = resolution.fallbackActive;
    m_boldFaceAvailable = resolution.boldFaceAvailable;
    // Looked up once here: fontForStyle() runs for every cell of every row.
    const NativeFontFace boldFace = m_boldFaceAvailable
        ? nearestNativeFace(m_effectivePrimary,
                            static_cast<QFont::Weight>(std::max(int(m_font.weight()), int(QFont::Bold))), true)
        : NativeFontFace{};
    m_boldStyleName = boldFace.style;
    m_boldWeight = boldFace.weight;
    updateMetrics();
}

void TerminalFontSet::setDevicePixelRatio(qreal ratio) {
    ratio = std::max<qreal>(1.0, ratio);
    if (qFuzzyCompare(m_devicePixelRatio, ratio)) {
        return;
    }
    m_devicePixelRatio = ratio;
    updateMetrics();
}

QFont TerminalFontSet::baseFont() const { return m_font; }

QFont TerminalFontSet::isolatedCopy(const QFont &font) {
    // setStyleHint() fully detaches (the new private data starts without
    // engine data), unlike setters that keep the shared engine data. Setting
    // the original hint back leaves an equal font.
    QFont copy(font);
    const QFont::StyleHint hint = copy.styleHint();
    const QFont::StyleStrategy strategy = copy.styleStrategy();
    copy.setStyleHint(hint == QFont::AnyStyle ? QFont::TypeWriter : QFont::AnyStyle, strategy);
    copy.setStyleHint(hint, strategy);
    return copy;
}

void TerminalFontSet::isolate() {
    m_requestedFont = isolatedCopy(m_requestedFont);
    m_font = isolatedCopy(m_font);
}

QFont TerminalFontSet::fontForStyle(const CellStyle &style) const {
    QFont styled = m_font;
    // Preserve the user's configured base weight. ANSI bold may only advance
    // to a heavier weight when the resolved primary family exposes a native
    // bold face; never synthesize bold or reset a configured light/medium face.
    if (style.bold && !m_boldStyleName.isEmpty()) {
        styled.setStyleName(m_boldStyleName);
        styled.setWeight(m_boldWeight);
    }
    styled.setItalic(style.italic);
    // Underline variants are painted explicitly by TerminalView so Qt does
    // not flatten double/curly underlines into a single line.
    styled.setUnderline(false);
    styled.setStrikeOut(style.strike);
    return styled;
}

qreal TerminalFontSet::cellWidth() const { return m_cellWidth; }
qreal TerminalFontSet::cellHeight() const { return m_cellHeight; }
qreal TerminalFontSet::baseline() const { return m_baseline; }
qreal TerminalFontSet::devicePixelRatio() const { return m_devicePixelRatio; }

bool TerminalFontSet::boldFaceAvailable() const {
    return m_boldFaceAvailable;
}

void TerminalFontSet::updateMetrics() {
    if (m_font.pixelSize() <= 0 && m_font.pointSizeF() <= 0) {
        m_font.setPixelSize(13);
    }
    const QFontMetricsF metrics(m_font);
    const qreal advance = std::max(metrics.horizontalAdvance(QStringLiteral("M")),
                                   metrics.horizontalAdvance(QStringLiteral("0")));
    m_cellWidth = std::max<qreal>(1.0, snapUp(advance, m_devicePixelRatio));
    // Resolve the font's own vertical metrics in physical pixels. The previous
    // extra two logical pixels grew with DPR and stretched every multi-row TUI
    // even when the font requested no leading.
    const qreal ascentPixels =
        std::ceil(std::max<qreal>(0.0, metrics.ascent()) * m_devicePixelRatio);
    const qreal descentPixels =
        std::ceil(std::max<qreal>(0.0, metrics.descent()) * m_devicePixelRatio);
    const qreal leadingPixels =
        std::ceil(std::max<qreal>(0.0, metrics.leading()) * m_devicePixelRatio);
    const qreal topLeadingPixels = std::floor(leadingPixels / 2.0);
    const qreal heightPixels =
        std::max<qreal>(1.0, ascentPixels + descentPixels + leadingPixels);
    m_cellHeight = heightPixels / m_devicePixelRatio;
    m_baseline = (topLeadingPixels + ascentPixels) / m_devicePixelRatio;
}

} // namespace span::terminal
