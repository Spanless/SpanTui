#pragma once

#include <QString>

class QQuickWindow;

namespace span::platform {

// The adapter a window's scene graph renders with.
struct GraphicsAdapter {
    QString name; // as the driver reports it; empty when unknown
    bool software = false; // a CPU rasterizer exposed as an adapter
};

// Read through the renderer's device handle, so only once the window has
// rendered. Other graphics APIs give an empty result.
//   Windows: the DXGI adapter behind Direct3D 11; WARP (the Microsoft Basic
//   Render Driver) is software.
//   macOS: the MTLDevice behind Metal.
GraphicsAdapter graphicsAdapter(QQuickWindow *window);

} // namespace span::platform
