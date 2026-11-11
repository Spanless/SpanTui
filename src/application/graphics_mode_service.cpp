#include "application/graphics_mode_service.h"

#include "application/application_constants.h"
#include "application/workspace_settings.h"

namespace span::application {

GraphicsModeService::GraphicsModeService(WorkspaceSettings *state, bool safeLaunchOption, bool interruptedStart,
                                         QObject *parent)
    : QObject(parent),
      m_state(state)
{
    const bool stored = state && state->renderMode() == kRenderSoftware;
    if (safeLaunchOption) {
        m_softwareReason = QStringLiteral("launchOption");
    } else if (interruptedStart && !stored) {
        m_softwareReason = QStringLiteral("interruptedStart");
    } else if (stored) {
        m_softwareReason = QStringLiteral("setting");
    }
    if (interruptedStart && state && !stored) {
        // Stored at once: if this launch dies too, the next one still knows.
        state->setRenderMode(kRenderSoftware);
    }
    if (state) {
        connect(state, &WorkspaceSettings::renderModeChanged, this, &GraphicsModeService::modeChanged);
    }
}

QString GraphicsModeService::mode() const
{
    return m_state ? m_state->renderMode() : QString(kRenderAuto);
}

void GraphicsModeService::setMode(const QString &mode)
{
    if (m_state)
        m_state->setRenderMode(mode);
}

bool GraphicsModeService::restartRequired() const
{
    // --safe-graphics holds for this launch only; it is not a pending change.
    const bool wanted = mode() == kRenderSoftware || m_softwareReason == QStringLiteral("launchOption");
    return wanted != softwareActive();
}

void GraphicsModeService::setRenderer(const QString &api, const QString &device, bool software)
{
    if (m_rendererApi == api && m_rendererDevice == device && m_rendererSoftware == software)
        return;
    m_rendererApi = api;
    m_rendererDevice = device;
    m_rendererSoftware = software;
    emit rendererChanged();
}

QString GraphicsModeService::fallbackNotice() const
{
    if (m_softwareReason != QStringLiteral("interruptedStart"))
        return {};
    return tr("The last start of SpanTui ended while its graphics were starting, so it now uses "
              "software rendering. If the graphics driver has been fixed, switch back in "
              "Settings → Environment → Rendering.");
}

} // namespace span::application
