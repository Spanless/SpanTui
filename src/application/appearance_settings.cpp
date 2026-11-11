#include "application/appearance_settings.h"

#include "application/settings_store.h"
#include "terminal/render/terminal_font_set.h"

#include <QFontDatabase>

#include <algorithm>
#include <limits>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

namespace span::application {

namespace {

QString matchAvailableFamily(const QStringList &families, const QString &requested)
{
    const auto match = std::find_if(families.cbegin(), families.cend(), [&requested](const QString &family) {
        return family.compare(requested, Qt::CaseInsensitive) == 0;
    });
    return match == families.cend() ? QString() : *match;
}

// Exact match first; then prefix match so "JetBrains Mono" can resolve
// "JetBrains Mono NL" / Nerd Font variants when the plain name is missing.
QString matchInstalledFamily(const QStringList &installed, const QString &requested)
{
    if (requested.trimmed().isEmpty()) {
        return {};
    }
    if (const QString exact = matchAvailableFamily(installed, requested); !exact.isEmpty()) {
        return exact;
    }
    const auto prefix = std::find_if(installed.cbegin(), installed.cend(), [&requested](const QString &family) {
        return family.startsWith(requested, Qt::CaseInsensitive);
    });
    return prefix == installed.cend() ? QString() : *prefix;
}

QString usableFamily(const QString &requested, const QStringList &families, const QString &preferred)
{
    const QString requestedMatch = matchAvailableFamily(families, requested);
    if (!requestedMatch.isEmpty()) return requestedMatch;
    const QString preferredMatch = matchAvailableFamily(families, preferred);
    if (!preferredMatch.isEmpty()) return preferredMatch;
    return families.isEmpty() ? preferred : families.first();
}

// Keep installed faces only; append platform fallbacks for missing CJK etc.
QStringList resolveInstalledFamilies(const QStringList &requested, const QStringList &platformFallbacks)
{
    const QStringList installed = QFontDatabase::families();
    QStringList resolved;
    const auto appendUnique = [&resolved](const QString &family) {
        if (!family.isEmpty() && !resolved.contains(family, Qt::CaseInsensitive)) {
            resolved.push_back(family);
        }
    };
    for (const QString &family : requested) {
        appendUnique(matchInstalledFamily(installed, family));
    }
    for (const QString &family : platformFallbacks) {
        appendUnique(matchInstalledFamily(installed, family));
    }
    if (resolved.isEmpty()) {
        appendUnique(QFont().defaultFamily());
    }
    return resolved;
}

struct NativeFontFace {
    QString style;
    int weight = int(QFont::Normal);
};

NativeFontFace nearestNativeFace(const QString &family, int requestedWeight)
{
    NativeFontFace nearest;
    int distance = std::numeric_limits<int>::max();
    for (const QString &style : QFontDatabase::styles(family)) {
        const QFont face = QFontDatabase::font(family, style, 14);
        if (face.italic()) {
            continue;
        }
        const int candidateDistance = qAbs(requestedWeight - int(face.weight()));
        if (candidateDistance < distance) {
            nearest.style = style;
            nearest.weight = int(face.weight());
            distance = candidateDistance;
        }
    }
    return nearest;
}

QFont fontFromNativeFace(const QStringList &families, int pixelSize, int requestedWeight)
{
    QFont font;
    if (families.isEmpty()) {
        font.setPixelSize(pixelSize);
        return font;
    }

    const NativeFontFace face = nearestNativeFace(families.constFirst(), requestedWeight);
    if (!face.style.isEmpty()) {
        font = QFontDatabase::font(families.constFirst(), face.style, pixelSize);
        font.setStyleName(face.style);
        font.setWeight(static_cast<QFont::Weight>(face.weight));
    }
    font.setFamilies(families);
    font.setPixelSize(pixelSize);
    return font;
}

QFont resolvedTerminalFont(const QString &primary, const QString &fallback, int pixelSize, int weight)
{
    QFont font;
    QStringList families{primary};
    if (!fallback.isEmpty() && fallback.compare(primary, Qt::CaseInsensitive) != 0) {
        families.push_back(fallback);
    }
    font.setFamilies(families);
    font.setPixelSize(pixelSize);
    font.setWeight(static_cast<QFont::Weight>(weight));
    font.setStyleHint(QFont::Monospace);
    font.setFixedPitch(true);
    font.setKerning(false);
    span::terminal::TerminalFontSet fonts;
    fonts.setFont(font);
    return fonts.baseFont();
}

} // namespace

AppearanceSettings::AppearanceSettings(SettingsStore *store, QObject *parent) : QObject(parent), m_store(store)
{
    for (int lane = 0; lane < FontLaneCount; ++lane) {
        const FontLaneSpec &spec = laneSpec(FontLaneId(lane));
        m_fontLanes[lane] = {QString::fromLatin1(spec.primary), QString::fromLatin1(spec.fallback), spec.defaultSize};
    }
    const auto bumpFontRevision = [this]() {
        ++m_fontRevision;
        emit fontRevisionChanged();
    };
    connect(this, &AppearanceSettings::appearanceChanged, this, bumpFontRevision);
    connect(this, &AppearanceSettings::terminalFontChanged, this, bumpFontRevision);
}

void AppearanceSettings::load()
{
    if (!m_store) {
        return;
    }
    const bool wasDark = effectiveDark();
    m_language = m_store->value(kSettingLanguage, defaultLanguage());
    if (m_language != kLanguageChinese && m_language != kLanguageEnglish) {
        m_language = defaultLanguage();
    }
    const QString storedThemeMode = m_store->value(kSettingThemeMode);
    m_themeMode =
        storedThemeMode == kThemeLight || storedThemeMode == kThemeDark ? storedThemeMode : QString(kThemeSystem);
    // An unset terminal family follows the bundled safe fixed-pitch face.
    const QString safeTerminalFamily = span::terminal::TerminalFontSet::safeFixedFont().family();
    for (int id = 0; id < FontLaneCount; ++id) {
        const FontLaneSpec &spec = laneSpec(FontLaneId(id));
        const QString key = QString::fromLatin1(spec.key);
        FontLane &lane = m_fontLanes[id];
        lane.primary = m_store->value(key + kSettingPrimaryFontFamilySuffix);
        if (lane.primary.isEmpty()) {
            lane.primary = id == TerminalLane ? safeTerminalFamily : QString::fromLatin1(spec.primary);
        }
        lane.fallback = m_store->value(key + kSettingFallbackFontFamilySuffix);
        if (lane.fallback.isEmpty()) {
            lane.fallback = id == TerminalLane ? safeTerminalFamily : QString::fromLatin1(spec.fallback);
        }
        lane.size = std::clamp(m_store->value(key + kSettingFontSizeSuffix, QString::number(spec.defaultSize)).toInt(),
                               spec.minSize, spec.maxSize);
        lane.weight = nearestNativeWeight(
            lane.primary, m_store->value(key + kSettingFontWeightSuffix, QStringLiteral("400")).toInt());
    }
    m_editorLineHeight = snappedLineHeight(m_store->value(kSettingEditorLineHeight, QStringLiteral("1")).toDouble());
    emit appearanceChanged();
    emit terminalFontChanged();
    if (wasDark != effectiveDark()) {
        emit effectiveDarkChanged();
    }
}

const AppearanceSettings::FontLaneSpec &AppearanceSettings::laneSpec(FontLaneId lane)
{
    static const FontLaneSpec specs[] = {
        {"ui", "Span Sans", "Microsoft YaHei UI", 14, 10, 18},
        {"uiAux", "Span Sans", "Microsoft YaHei UI", 12, 8, 16},
        {"formHint", "Span Sans", "Microsoft YaHei UI", 10, 8, 16},
        {"editor", "Span Mono", "Microsoft YaHei Mono", 13, 10, 22},
        {"terminal", "Span Mono", "Cascadia Mono", 13, 10, 22},
    };
    return specs[lane];
}

int AppearanceSettings::nearestNativeWeight(const QString &family, int requested)
{
    return nearestNativeFace(family, requested).weight;
}

double AppearanceSettings::snappedLineHeight(double value)
{
    static const double kOptions[] = {1.0, 1.25, 1.5, 2.0};
    double nearest = kOptions[0];
    for (double option : kOptions) {
        if (qAbs(value - option) < qAbs(value - nearest)) {
            nearest = option;
        }
    }
    return nearest;
}

void AppearanceSettings::store(const QString &key, const QString &value)
{
    if (m_store) {
        m_store->setValue(key, value);
    }
}

bool AppearanceSettings::effectiveDark() const
{
    if (m_themeMode == kThemeDark) {
        return true;
    }
    if (m_themeMode == kThemeLight) {
        return false;
    }
    return m_systemDark;
}

void AppearanceSettings::refreshSystemReduceMotion()
{
    bool reduce = false;
    // SPI_GETCLIENTAREAANIMATION: FALSE means the user turned off window
    // animations (Accessibility → Visual effects → Animation effects).
    BOOL clientAreaAnimation = TRUE;
    if (SystemParametersInfoW(SPI_GETCLIENTAREAANIMATION, 0, &clientAreaAnimation, 0) && !clientAreaAnimation) {
        reduce = true;
    }
    if (m_reduceMotion == reduce) {
        return;
    }
    m_reduceMotion = reduce;
    emit reduceMotionChanged();
}

void AppearanceSettings::ensureFontCaches() const
{
    if (m_fontCacheReady) {
        return;
    }
    m_allFonts = span::terminal::TerminalFontSet::availableFamilies(false);
    m_fixedPitchFonts = span::terminal::TerminalFontSet::availableFamilies(true);
    m_fontCacheReady = true;
}

QStringList AppearanceSettings::availableFonts() const
{
    ensureFontCaches();
    return m_allFonts;
}

QStringList AppearanceSettings::availableFixedPitchFonts() const
{
    ensureFontCaches();
    return m_fixedPitchFonts;
}

// Editor primaries are fixed-pitch only; every other pick is any face.
QStringList AppearanceSettings::laneFamilyChoices(FontLaneId lane, bool primary) const
{
    return primary && lane == EditorLane ? availableFixedPitchFonts() : availableFonts();
}

QStringList AppearanceSettings::laneFamilyList(FontLaneId lane) const
{
    static const QStringList kUiTails{QStringLiteral("Segoe UI Variable Text"), QStringLiteral("Segoe UI"),
                                      QStringLiteral("Microsoft YaHei UI"),     QStringLiteral("Microsoft YaHei"),
                                      QStringLiteral("Noto Sans SC"),           QStringLiteral("Arial")};
    static const QStringList kEditorTails{QStringLiteral("Cascadia Mono"),        QStringLiteral("Consolas"),
                                          QStringLiteral("Microsoft YaHei Mono"), QStringLiteral("Microsoft YaHei"),
                                          QStringLiteral("Noto Sans SC"),         QStringLiteral("Courier New")};
    const FontLane &font = m_fontLanes[lane];
    return resolveInstalledFamilies({font.primary, font.fallback}, lane == EditorLane ? kEditorTails : kUiTails);
}

QFont AppearanceSettings::laneFont(FontLaneId lane, int pixelSize) const
{
    const FontLane &spec = m_fontLanes[lane];
    const int size = std::max(8, pixelSize);
    if (lane == TerminalLane) {
        return resolvedTerminalFont(spec.primary, spec.fallback, size, spec.weight);
    }
    QFont font = fontFromNativeFace(laneFamilyList(lane), size, spec.weight);
    if (lane == EditorLane) {
        font.setStyleHint(QFont::Monospace, QFont::PreferMatch);
        font.setFixedPitch(true);
        font.setKerning(false);
    } else {
        // Do not force SansSerif: the user may set a mono face as the UI font.
        font.setKerning(true);
    }
    return font;
}

QFont AppearanceSettings::uiFont(int pixelSize, int) const
{
    return laneFont(UiLane, pixelSize);
}
QFont AppearanceSettings::uiAuxFont(int pixelSize, int) const
{
    return laneFont(UiAuxLane, pixelSize);
}
QFont AppearanceSettings::formHintFont(int pixelSize, int) const
{
    return laneFont(FormHintLane, pixelSize);
}
QFont AppearanceSettings::editorFont(int pixelSize, int) const
{
    return laneFont(EditorLane, pixelSize);
}

QVariantList AppearanceSettings::availableFontWeights(FontLaneId lane) const
{
    const QString &family = m_fontLanes[lane].primary;
    QVariantList result;
    QList<int> seenWeights;
    for (const QString &style : QFontDatabase::styles(family)) {
        const QFont face = QFontDatabase::font(family, style, 14);
        const int weight = int(face.weight());
        if (face.italic() || seenWeights.contains(weight)) {
            continue;
        }
        seenWeights.push_back(weight);
        result.push_back(QVariantMap{{QStringLiteral("weight"), weight}, {QStringLiteral("style"), style}});
    }
    std::sort(result.begin(), result.end(), [](const QVariant &left, const QVariant &right) {
        return left.toMap().value(QStringLiteral("weight")).toInt()
            < right.toMap().value(QStringLiteral("weight")).toInt();
    });
    if (result.isEmpty()) {
        result.push_back(QVariantMap{{QStringLiteral("weight"), int(QFont::Normal)},
                                     {QStringLiteral("style"), QStringLiteral("Regular")}});
    }
    return result;
}

void AppearanceSettings::storeLane(FontLaneId lane)
{
    const QString key = QString::fromLatin1(laneSpec(lane).key);
    const FontLane &font = m_fontLanes[lane];
    store(key + kSettingPrimaryFontFamilySuffix, font.primary);
    store(key + kSettingFallbackFontFamilySuffix, font.fallback);
    store(key + kSettingFontSizeSuffix, QString::number(font.size));
    store(key + kSettingFontWeightSuffix, QString::number(font.weight));
    if (lane == TerminalLane) {
        emit terminalFontChanged();
    } else {
        emit appearanceChanged();
    }
}

void AppearanceSettings::setLanePrimary(FontLaneId lane, const QString &value)
{
    FontLane &font = m_fontLanes[lane];
    const QString normalized =
        usableFamily(value, laneFamilyChoices(lane, true), QString::fromLatin1(laneSpec(lane).primary));
    if (font.primary == normalized) {
        return;
    }
    font.primary = normalized;
    font.weight = nearestNativeWeight(normalized, font.weight);
    storeLane(lane);
}

void AppearanceSettings::setLaneFallback(FontLaneId lane, const QString &value)
{
    FontLane &font = m_fontLanes[lane];
    const QString normalized =
        usableFamily(value, laneFamilyChoices(lane, false), QString::fromLatin1(laneSpec(lane).fallback));
    if (font.fallback == normalized) {
        return;
    }
    font.fallback = normalized;
    storeLane(lane);
}

void AppearanceSettings::setLaneSize(FontLaneId lane, int value)
{
    const auto &spec = laneSpec(lane);
    const int clamped = std::clamp(value, spec.minSize, spec.maxSize);
    if (m_fontLanes[lane].size == clamped) {
        return;
    }
    m_fontLanes[lane].size = clamped;
    storeLane(lane);
}

void AppearanceSettings::setLaneWeight(FontLaneId lane, int value)
{
    FontLane &font = m_fontLanes[lane];
    const int normalized = nearestNativeWeight(font.primary, value);
    if (font.weight == normalized) {
        return;
    }
    font.weight = normalized;
    storeLane(lane);
}

bool AppearanceSettings::isFixedPitch(const QString &family) const
{
    return family.isEmpty() || QFontDatabase::isFixedPitch(family);
}

void AppearanceSettings::setLanguage(const QString &value)
{
    if (m_language == value) return;
    m_language = value;
    store(kSettingLanguage, m_language);
    emit appearanceChanged();
}

void AppearanceSettings::setThemeMode(const QString &value)
{
    const QString normalized = value == kThemeDark || value == kThemeLight ? value : QString(kThemeSystem);
    if (m_themeMode == normalized) return;
    store(kSettingThemeMode, normalized);
    setEffectiveDarkInputs(normalized, m_systemDark);
}

void AppearanceSettings::setSystemDark(bool value)
{
    if (m_systemDark == value) return;
    setEffectiveDarkInputs(m_themeMode, value);
}

void AppearanceSettings::setEffectiveDarkInputs(const QString &themeMode, bool systemDark)
{
    const bool wasDark = effectiveDark();
    const bool themeChanged = m_themeMode != themeMode;
    m_themeMode = themeMode;
    m_systemDark = systemDark;
    const bool darkChanged = wasDark != effectiveDark();
    if (themeChanged || darkChanged) {
        emit appearanceChanged();
    }
    if (darkChanged) {
        emit effectiveDarkChanged();
    }
}

void AppearanceSettings::setSystemWheelScrollLines(int value)
{
    if (m_wheelScrollLines == value) {
        return;
    }
    m_wheelScrollLines = value;
    emit wheelScrollLinesChanged();
}

void AppearanceSettings::setEditorLineHeight(double value)
{
    const double snapped = snappedLineHeight(value);
    if (qFuzzyCompare(m_editorLineHeight + 1.0, snapped + 1.0)) {
        return;
    }
    m_editorLineHeight = snapped;
    store(kSettingEditorLineHeight, QString::number(m_editorLineHeight, 'g', 4));
    emit appearanceChanged();
}

} // namespace span::application
