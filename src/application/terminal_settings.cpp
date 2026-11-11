#include "application/terminal_settings.h"

#include "application/appearance_settings.h"
#include "application/application_constants.h"
#include "application/settings_store.h"

#include <QJsonDocument>
#include <QJsonObject>

#include <algorithm>

namespace span::application {

namespace {

constexpr int kMaximumColorSchemeNameLength = 64;

QColor colorFromVariant(const QVariant &value)
{
    const QColor direct = value.value<QColor>();
    if (direct.isValid()) {
        return direct;
    }
    return QColor(value.toString());
}

bool parseColorScheme(const QString &name, const QVariantMap &map, span::terminal::TerminalColorScheme *out)
{
    span::terminal::TerminalColorScheme scheme;
    scheme.name = name;
    scheme.foreground = colorFromVariant(map.value(QStringLiteral("foreground")));
    scheme.background = colorFromVariant(map.value(QStringLiteral("background")));
    scheme.cursorColor = colorFromVariant(map.value(QStringLiteral("cursor")));
    scheme.selectionBackground = colorFromVariant(map.value(QStringLiteral("selection")));
    scheme.uiBackground = colorFromVariant(map.value(QStringLiteral("uiBackground")));
    scheme.uiPlate = colorFromVariant(map.value(QStringLiteral("uiPlate")));
    scheme.uiAccent = colorFromVariant(map.value(QStringLiteral("uiAccent")));
    scheme.uiHover = colorFromVariant(map.value(QStringLiteral("uiHover")));
    scheme.uiSelection = colorFromVariant(map.value(QStringLiteral("uiSelection")));
    scheme.uiPressed = colorFromVariant(map.value(QStringLiteral("uiPressed")));
    const QVariantList ansi = map.value(QStringLiteral("ansi")).toList();
    for (const QVariant &color : ansi) {
        scheme.ansi.append(colorFromVariant(color));
    }
    if (!scheme.isValid()) {
        return false;
    }
    span::terminal::completeSchemeUi(&scheme);
    *out = scheme;
    return true;
}

QVariantMap colorSchemeToMap(const span::terminal::TerminalColorScheme &scheme, bool builtIn)
{
    QVariantMap map;
    map.insert(QStringLiteral("name"), scheme.name);
    map.insert(QStringLiteral("builtIn"), builtIn);
    map.insert(QStringLiteral("foreground"), scheme.foreground);
    map.insert(QStringLiteral("background"), scheme.background);
    map.insert(QStringLiteral("cursor"), scheme.cursorColor);
    map.insert(QStringLiteral("selection"), scheme.selectionBackground);
    map.insert(QStringLiteral("uiBackground"), scheme.uiBackground);
    map.insert(QStringLiteral("uiPlate"), scheme.uiPlate);
    map.insert(QStringLiteral("uiAccent"), scheme.uiAccent);
    map.insert(QStringLiteral("uiHover"), scheme.uiHover);
    map.insert(QStringLiteral("uiSelection"), scheme.uiSelection);
    map.insert(QStringLiteral("uiPressed"), scheme.uiPressed);
    QVariantList ansi;
    ansi.reserve(scheme.ansi.size());
    for (const QColor &color : scheme.ansi) {
        ansi.append(color);
    }
    map.insert(QStringLiteral("ansi"), ansi);
    return map;
}

bool isBuiltinColorSchemeName(const QString &name)
{
    const auto builtins = span::terminal::builtinColorSchemes();
    return std::any_of(builtins.cbegin(), builtins.cend(),
                       [&name](const span::terminal::TerminalColorScheme &scheme) { return scheme.name == name; });
}

QString normalizedCursorStyle(const QString &value)
{
    return value == span::terminal::kCursorStyleBar || value == span::terminal::kCursorStyleUnderline
        ? value
        : QString(span::terminal::kCursorStyleBlock);
}

QString compactJson(const QVariantMap &map)
{
    return QString::fromUtf8(QJsonDocument(QJsonObject::fromVariantMap(map)).toJson(QJsonDocument::Compact));
}

} // namespace

TerminalSettings::TerminalSettings(SettingsStore *store, const AppearanceSettings *appearance, QObject *parent)
    : QObject(parent),
      m_store(store),
      m_appearance(appearance)
{
    if (m_appearance) {
        connect(m_appearance, &AppearanceSettings::effectiveDarkChanged, this, &TerminalSettings::appearanceChanged);
    }
}

void TerminalSettings::load()
{
    if (!m_store) {
        return;
    }
    m_userColorSchemes.clear();
    const QJsonDocument stored =
        QJsonDocument::fromJson(m_store->value(kSettingTerminalColorSchemes, QStringLiteral("{}")).toUtf8());
    if (stored.isObject()) {
        const QJsonObject object = stored.object();
        for (auto it = object.begin(); it != object.end(); ++it) {
            span::terminal::TerminalColorScheme scheme;
            if (span::terminal::schemeFromJson(it.key(), it.value().toObject(), &scheme)) {
                m_userColorSchemes.append(scheme);
            }
        }
    }
    const QString darkSlot = m_store->value(kSettingTerminalColorSchemeDark);
    const QString lightSlot = m_store->value(kSettingTerminalColorSchemeLight);
    m_colorSchemeDark = findColorScheme(darkSlot) ? darkSlot : QString(span::terminal::kDefaultDarkColorScheme);
    m_colorSchemeLight = findColorScheme(lightSlot) ? lightSlot : QString(span::terminal::kDefaultLightColorScheme);
    m_cursorStyle = normalizedCursorStyle(m_store->value(kSettingTerminalCursorStyle));
    bool refreshRateOk = false;
    const int storedRefreshRate = m_store->value(kSettingTerminalRefreshRate).toInt(&refreshRateOk);
    m_refreshRate = refreshRateOk
        ? std::clamp(storedRefreshRate, span::terminal::kMinimumFrameRate, span::terminal::kMaximumFrameRate)
        : span::terminal::kDefaultFrameRate;
    m_terminateOnTileClose =
        m_store->value(kSettingWorkspaceTerminateOnTileClose, QStringLiteral("1")) == QStringLiteral("1");
    const QJsonDocument storedShells =
        QJsonDocument::fromJson(m_store->value(kSettingTerminalShellProfiles, QStringLiteral("{}")).toUtf8());
    m_shellProfiles = storedShells.isObject() ? storedShells.object().toVariantMap() : QVariantMap();
    emit appearanceChanged();
    emit colorSchemesChanged();
    emit closePreferenceChanged();
    emit shellProfilesChanged();
}

void TerminalSettings::store(const QString &key, const QString &value)
{
    if (m_store) {
        m_store->setValue(key, value);
    }
}

std::optional<span::terminal::TerminalColorScheme> TerminalSettings::findColorScheme(const QString &name) const
{
    if (!name.isEmpty()) {
        for (const span::terminal::TerminalColorScheme &scheme : span::terminal::builtinColorSchemes()) {
            if (scheme.name == name) {
                return scheme;
            }
        }
        for (const span::terminal::TerminalColorScheme &scheme : m_userColorSchemes) {
            if (scheme.name == name) {
                return scheme;
            }
        }
    }
    return std::nullopt;
}

span::terminal::TerminalColorScheme TerminalSettings::activeColorScheme() const
{
    const bool dark = m_appearance && m_appearance->effectiveDark();
    if (const auto scheme = findColorScheme(dark ? m_colorSchemeDark : m_colorSchemeLight)) {
        return *scheme;
    }
    return span::terminal::builtinColorSchemes().constFirst();
}

QVariantMap TerminalSettings::surfaceColors() const
{
    const span::terminal::TerminalColorScheme scheme = activeColorScheme();
    return colorSchemeToMap(scheme, isBuiltinColorSchemeName(scheme.name));
}

QVariantList TerminalSettings::colorSchemes() const
{
    QVariantList list;
    for (const span::terminal::TerminalColorScheme &scheme : span::terminal::builtinColorSchemes()) {
        list.append(colorSchemeToMap(scheme, true));
    }
    for (const span::terminal::TerminalColorScheme &scheme : m_userColorSchemes) {
        list.append(colorSchemeToMap(scheme, false));
    }
    return list;
}

QVariantMap TerminalSettings::colorScheme(const QString &name) const
{
    if (const auto scheme = findColorScheme(name)) {
        return colorSchemeToMap(*scheme, isBuiltinColorSchemeName(name));
    }
    return {};
}

void TerminalSettings::storeUserColorSchemes()
{
    QJsonObject schemes;
    for (const span::terminal::TerminalColorScheme &scheme : std::as_const(m_userColorSchemes)) {
        schemes.insert(scheme.name, span::terminal::schemeToJson(scheme));
    }
    store(kSettingTerminalColorSchemes, QString::fromUtf8(QJsonDocument(schemes).toJson(QJsonDocument::Compact)));
    emit colorSchemesChanged();
    emit appearanceChanged();
}

bool TerminalSettings::saveColorScheme(const QString &name, const QVariantMap &scheme)
{
    const QString trimmed = name.trimmed();
    if (trimmed.isEmpty() || trimmed.size() > kMaximumColorSchemeNameLength) {
        emit errorOccurred(tr("Color scheme name must be 1-64 characters."));
        return false;
    }
    if (isBuiltinColorSchemeName(trimmed)) {
        emit errorOccurred(tr("“%1” is a built-in color scheme; save it under a new name.").arg(trimmed));
        return false;
    }
    span::terminal::TerminalColorScheme parsed;
    if (!parseColorScheme(trimmed, scheme, &parsed)) {
        emit errorOccurred(tr("A color scheme needs four surface colors and 16 valid ANSI colors."));
        return false;
    }
    const auto existing =
        std::find_if(m_userColorSchemes.begin(), m_userColorSchemes.end(),
                     [&trimmed](const span::terminal::TerminalColorScheme &s) { return s.name == trimmed; });
    if (existing != m_userColorSchemes.end()) {
        *existing = parsed;
    } else {
        m_userColorSchemes.append(parsed);
    }
    storeUserColorSchemes();
    return true;
}

bool TerminalSettings::removeColorScheme(const QString &name)
{
    if (isBuiltinColorSchemeName(name)) {
        emit errorOccurred(tr("Built-in color schemes cannot be deleted."));
        return false;
    }
    const auto existing =
        std::find_if(m_userColorSchemes.begin(), m_userColorSchemes.end(),
                     [&name](const span::terminal::TerminalColorScheme &s) { return s.name == name; });
    if (existing == m_userColorSchemes.end()) {
        emit errorOccurred(tr("Unknown color scheme: %1").arg(name));
        return false;
    }
    if (name == m_colorSchemeDark || name == m_colorSchemeLight) {
        emit errorOccurred(tr("“%1” is in use by a theme slot; switch the slot first.").arg(name));
        return false;
    }
    m_userColorSchemes.erase(existing);
    storeUserColorSchemes();
    return true;
}

void TerminalSettings::setColorSchemeDark(const QString &value)
{
    assignColorScheme(m_colorSchemeDark, value, kSettingTerminalColorSchemeDark);
}

void TerminalSettings::setColorSchemeLight(const QString &value)
{
    assignColorScheme(m_colorSchemeLight, value, kSettingTerminalColorSchemeLight);
}

void TerminalSettings::assignColorScheme(QString &slot, const QString &value, const QString &key)
{
    const QString name = value.trimmed();
    if (!findColorScheme(name)) {
        emit errorOccurred(tr("Unknown terminal color scheme: %1").arg(name));
        return;
    }
    if (slot == name) {
        return;
    }
    slot = name;
    store(key, name);
    emit appearanceChanged();
}

void TerminalSettings::setCursorStyle(const QString &value)
{
    const QString normalized = normalizedCursorStyle(value);
    if (m_cursorStyle == normalized) {
        return;
    }
    m_cursorStyle = normalized;
    store(kSettingTerminalCursorStyle, m_cursorStyle);
    emit appearanceChanged();
}

void TerminalSettings::setRefreshRate(int value)
{
    const int normalized = std::clamp(value, span::terminal::kMinimumFrameRate, span::terminal::kMaximumFrameRate);
    if (m_refreshRate == normalized) {
        return;
    }
    m_refreshRate = normalized;
    store(kSettingTerminalRefreshRate, QString::number(m_refreshRate));
    emit appearanceChanged();
}

void TerminalSettings::setTerminateOnTileClose(bool value)
{
    if (m_terminateOnTileClose == value) {
        return;
    }
    m_terminateOnTileClose = value;
    store(kSettingWorkspaceTerminateOnTileClose, value ? QStringLiteral("1") : QStringLiteral("0"));
    emit closePreferenceChanged();
}

void TerminalSettings::setShellProfiles(const QVariantMap &value)
{
    if (m_shellProfiles == value) {
        return;
    }
    m_shellProfiles = value;
    store(kSettingTerminalShellProfiles, compactJson(m_shellProfiles));
    emit shellProfilesChanged();
}

} // namespace span::application
