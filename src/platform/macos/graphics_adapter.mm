#include "platform/graphics_adapter.h"

#include <QQuickWindow>
#include <QSGRendererInterface>

#import <Metal/Metal.h>

namespace span::platform {

GraphicsAdapter graphicsAdapter(QQuickWindow *window)
{
    GraphicsAdapter adapter;
    QSGRendererInterface *renderer = window ? window->rendererInterface() : nullptr;
    if (!renderer || renderer->graphicsApi() != QSGRendererInterface::Metal) return adapter;
    void *handle = renderer->getResource(window, QSGRendererInterface::DeviceResource);
    if (!handle) return adapter;
    const id<MTLDevice> device = (__bridge id<MTLDevice>)handle;
    adapter.name = QString::fromNSString(device.name);
    return adapter;
}

} // namespace span::platform
