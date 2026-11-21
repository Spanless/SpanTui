#include "platform/graphics_adapter.h"

#include <QQuickWindow>
#include <QSGRendererInterface>

#include <d3d11.h>
#include <dxgi.h>

namespace span::platform {
namespace {

// WARP reports Microsoft's vendor id and the Basic Render Driver device id.
constexpr UINT kMicrosoftVendorId = 0x1414;
constexpr UINT kBasicRenderDriverDeviceId = 0x8c;

} // namespace

GraphicsAdapter graphicsAdapter(QQuickWindow *window)
{
    GraphicsAdapter adapter;
    QSGRendererInterface *renderer = window ? window->rendererInterface() : nullptr;
    if (!renderer || renderer->graphicsApi() != QSGRendererInterface::Direct3D11) return adapter;
    auto *device = static_cast<ID3D11Device *>(renderer->getResource(window, QSGRendererInterface::DeviceResource));
    if (!device) return adapter;
    IDXGIDevice *dxgiDevice = nullptr;
    if (FAILED(device->QueryInterface(__uuidof(IDXGIDevice), reinterpret_cast<void **>(&dxgiDevice)))) return adapter;
    IDXGIAdapter *dxgiAdapter = nullptr;
    if (SUCCEEDED(dxgiDevice->GetAdapter(&dxgiAdapter))) {
        DXGI_ADAPTER_DESC description{};
        if (SUCCEEDED(dxgiAdapter->GetDesc(&description))) {
            adapter.name = QString::fromWCharArray(description.Description);
            adapter.software =
                description.VendorId == kMicrosoftVendorId && description.DeviceId == kBasicRenderDriverDeviceId;
        }
        dxgiAdapter->Release();
    }
    dxgiDevice->Release();
    return adapter;
}

} // namespace span::platform
