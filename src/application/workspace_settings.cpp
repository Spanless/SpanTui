#include "application/workspace_settings.h"

#include "application/settings_store.h"

#include <QJsonDocument>
#include <QJsonObject>

#include <algorithm>

namespace span::application {

namespace {

QString normalizedScriptRunMode(const QString &value)
{
    return value == kScriptRunIndependentWindow ? QString(kScriptRunIndependentWindow)
                                                : QString(kScriptRunProjectSession);
}

QString normalizedRenderMode(const QString &value)
{
    return value == kRenderSoftware ? QString(kRenderSoftware) : QString(kRenderAuto);
}

// String entries of a stored JSON object; `keepEmpty` keeps "" values.
QVariantMap storedStringMap(const QString &json, bool keepEmpty)
{
    QVariantMap map;
    const QJsonDocument document = QJsonDocument::fromJson(json.toUtf8());
    if (!document.isObject()) {
        return map;
    }
    const QJsonObject object = document.object();
    for (auto it = object.begin(); it != object.end(); ++it) {
        if (it.value().isString() && (keepEmpty || !it.value().toString().isEmpty())) {
            map.insert(it.key(), it.value().toString());
        }
    }
    return map;
}

QString compactJson(const QVariantMap &map)
{
    return QString::fromUtf8(QJsonDocument(QJsonObject::fromVariantMap(map)).toJson(QJsonDocument::Compact));
}

} // namespace

WorkspaceSettings::WorkspaceSettings(SettingsStore *store, QObject *parent) : QObject(parent), m_store(store) {}

void WorkspaceSettings::load()
{
    if (!m_store) {
        return;
    }
    m_scriptRunMode = normalizedScriptRunMode(m_store->value(kSettingWorkspaceScriptRunMode));
    m_renderMode = normalizedRenderMode(m_store->value(kSettingRenderMode));
    m_performanceMonitorIntervalSeconds = std::clamp(
        m_store->value(kSettingMonitorIntervalSeconds, QString::number(kMonitorIntervalDefaultSeconds)).toInt(),
        kMonitorIntervalMinSeconds, kMonitorIntervalMaxSeconds);
    // Unknown ids and unparsable sequences are kept and written back verbatim
    // (a newer build's entries survive an older one); the registry ignores
    // what it cannot use.
    m_shortcutOverrides = storedStringMap(m_store->value(kSettingShortcutBindings, QStringLiteral("{}")), true);
    m_toolPathOverrides = storedStringMap(m_store->value(kSettingToolPaths, QStringLiteral("{}")), false);
    emit scriptRunModeChanged();
    emit renderModeChanged();
    emit performanceMonitorIntervalChanged();
    emit shortcutOverridesChanged();
    emit toolPathOverridesChanged();
}

void WorkspaceSettings::store(const QString &key, const QString &value)
{
    if (m_store) {
        m_store->setValue(key, value);
    }
}

void WorkspaceSettings::setScriptRunMode(const QString &value)
{
    const QString normalized = normalizedScriptRunMode(value);
    if (m_scriptRunMode == normalized) {
        return;
    }
    m_scriptRunMode = normalized;
    store(kSettingWorkspaceScriptRunMode, m_scriptRunMode);
    emit scriptRunModeChanged();
}

void WorkspaceSettings::setRenderMode(const QString &value)
{
    const QString normalized = normalizedRenderMode(value);
    if (m_renderMode == normalized) {
        return;
    }
    m_renderMode = normalized;
    store(kSettingRenderMode, m_renderMode);
    // Written at once: a launch that dies while its graphics start must
    // still leave the choice for the next one.
    if (m_store) {
        m_store->flush();
    }
    emit renderModeChanged();
}

void WorkspaceSettings::setPerformanceMonitorIntervalSeconds(int value)
{
    const int clamped = std::clamp(value, kMonitorIntervalMinSeconds, kMonitorIntervalMaxSeconds);
    if (m_performanceMonitorIntervalSeconds == clamped) {
        return;
    }
    m_performanceMonitorIntervalSeconds = clamped;
    store(kSettingMonitorIntervalSeconds, QString::number(clamped));
    emit performanceMonitorIntervalChanged();
}

void WorkspaceSettings::setShortcutOverrides(const QVariantMap &value)
{
    if (m_shortcutOverrides == value) {
        return;
    }
    m_shortcutOverrides = value;
    store(kSettingShortcutBindings, compactJson(m_shortcutOverrides));
    emit shortcutOverridesChanged();
}

void WorkspaceSettings::setToolPathOverrides(const QVariantMap &value)
{
    if (m_toolPathOverrides == value) {
        return;
    }
    m_toolPathOverrides = value;
    store(kSettingToolPaths, compactJson(m_toolPathOverrides));
    emit toolPathOverridesChanged();
}

} // namespace span::application
