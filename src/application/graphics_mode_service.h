#pragma once

#include <QObject>
#include <QPointer>
#include <QString>

namespace span::application {

class WorkspaceSettings;

// Owns the per-launch rendering choice: hardware or software (WARP).
//
// The stored preference is WorkspaceSettings::renderMode. A launch also uses software
// rendering when started with --safe-graphics (this launch only) or when the
// previous launch died while its graphics came up — that case also stores
// "software", so a crashing driver does not take every other launch down.
// The choice is fixed for the process; changing the preference applies at
// the next start.
class GraphicsModeService final : public QObject {
    Q_OBJECT
    // Stored preference: "auto" | "software".
    Q_PROPERTY(QString mode READ mode WRITE setMode NOTIFY modeChanged FINAL)
    Q_PROPERTY(bool softwareActive READ softwareActive CONSTANT FINAL)
    // Why this launch renders in software: "setting" | "launchOption" |
    // "interruptedStart"; empty for hardware.
    Q_PROPERTY(QString softwareReason READ softwareReason CONSTANT FINAL)
    Q_PROPERTY(bool restartRequired READ restartRequired NOTIFY modeChanged FINAL)
    // Renderer the main window ended up with; empty until its first frame.
    Q_PROPERTY(QString rendererApi READ rendererApi NOTIFY rendererChanged FINAL)
    Q_PROPERTY(QString rendererDevice READ rendererDevice NOTIFY rendererChanged FINAL)
    Q_PROPERTY(bool rendererSoftware READ rendererSoftware NOTIFY rendererChanged FINAL)

public:
    GraphicsModeService(WorkspaceSettings *state, bool safeLaunchOption, bool interruptedStart,
                        QObject *parent = nullptr);

    QString mode() const;
    void setMode(const QString &mode);
    bool softwareActive() const { return !m_softwareReason.isEmpty(); }
    QString softwareReason() const { return m_softwareReason; }
    bool restartRequired() const;

    QString rendererApi() const { return m_rendererApi; }
    QString rendererDevice() const { return m_rendererDevice; }
    bool rendererSoftware() const { return m_rendererSoftware; }
    void setRenderer(const QString &api, const QString &device, bool software);

    // User-facing notice for a launch switched to software rendering by an
    // interrupted start; empty otherwise. Read after the translator loads.
    QString fallbackNotice() const;

signals:
    void modeChanged();
    void rendererChanged();

private:
    QPointer<WorkspaceSettings> m_state;
    QString m_softwareReason;
    QString m_rendererApi;
    QString m_rendererDevice;
    bool m_rendererSoftware = false;
};

} // namespace span::application
