#pragma once

#include "application/application_constants.h"

#include <QObject>
#include <QPointer>
#include <QVariantMap>

namespace span::application {

class SettingsStore;

// Workspace behavior preferences (QML singleton `WorkspaceSettings`).
class WorkspaceSettings final : public QObject {
    Q_OBJECT
    Q_PROPERTY(QString scriptRunMode READ scriptRunMode WRITE setScriptRunMode NOTIFY scriptRunModeChanged FINAL)
    // "auto" (hardware, Qt picks WARP only when no device starts) or
    // "software" (always WARP). Read once per launch; see GraphicsModeService.
    Q_PROPERTY(QString renderMode READ renderMode WRITE setRenderMode NOTIFY renderModeChanged FINAL)
    Q_PROPERTY(int performanceMonitorIntervalSeconds READ performanceMonitorIntervalSeconds WRITE
                   setPerformanceMonitorIntervalSeconds NOTIFY performanceMonitorIntervalChanged FINAL)
    // User shortcut overrides (action id -> portable key sequence, "" unbinds).
    // ShortcutRegistry owns the catalog and validates before writing here.
    Q_PROPERTY(QVariantMap shortcutOverrides READ shortcutOverrides NOTIFY shortcutOverridesChanged FINAL)
    // User-chosen executables (tool id -> absolute path). ToolRegistry owns
    // detection and validation and writes here; absent ids are auto-detected.
    Q_PROPERTY(QVariantMap toolPathOverrides READ toolPathOverrides NOTIFY toolPathOverridesChanged FINAL)

public:
    explicit WorkspaceSettings(SettingsStore *store = nullptr, QObject *parent = nullptr);

    // Reads every key from the store; call once after it opened.
    void load();

    QString scriptRunMode() const { return m_scriptRunMode; }
    QString renderMode() const { return m_renderMode; }
    int performanceMonitorIntervalSeconds() const { return m_performanceMonitorIntervalSeconds; }
    QVariantMap shortcutOverrides() const { return m_shortcutOverrides; }
    QVariantMap toolPathOverrides() const { return m_toolPathOverrides; }

    void setScriptRunMode(const QString &value);
    void setRenderMode(const QString &value);
    void setPerformanceMonitorIntervalSeconds(int value);
    void setShortcutOverrides(const QVariantMap &value);
    void setToolPathOverrides(const QVariantMap &value);

signals:
    void scriptRunModeChanged();
    void renderModeChanged();
    void performanceMonitorIntervalChanged();
    void shortcutOverridesChanged();
    void toolPathOverridesChanged();

private:
    void store(const QString &key, const QString &value);

    QPointer<SettingsStore> m_store;
    QString m_scriptRunMode = kScriptRunProjectSession;
    QString m_renderMode = kRenderAuto;
    int m_performanceMonitorIntervalSeconds = kMonitorIntervalDefaultSeconds;
    QVariantMap m_shortcutOverrides;
    QVariantMap m_toolPathOverrides;
};

} // namespace span::application
