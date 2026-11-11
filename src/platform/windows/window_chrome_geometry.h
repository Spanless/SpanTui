#pragma once

#include <QPoint>
#include <QRect>
#include <QSize>
#include <QVector>

// Pure hit-testing / geometry math for the frameless window chrome.
// Deliberately free of any Win32 types so it can be unit tested; WindowChrome
// translates HitRegion values to the corresponding WM_NCHITTEST codes.
namespace window_chrome_geometry {

enum class HitRegion {
    Nowhere, // outside client — must not steal taskbar / desktop clicks
    Client,
    Caption,
    MaxButton,
    Left,
    Right,
    Top,
    Bottom,
    TopLeft,
    TopRight,
    BottomLeft,
    BottomRight,
};

// All inputs share one coordinate space (client-area physical pixels).
// resizeBand is ignored while maximized — a maximized window has no resize edges.
HitRegion hitTest(const QPoint &pos,
                  const QSize &clientSize,
                  int resizeBand,
                  int titleBarHeight,
                  const QRect &maxButtonRect,
                  const QVector<QRect> &interactiveRects,
                  bool maximized);

// Fits a restored-window rect onto the virtual desktop so a geometry saved on
// a monitor that no longer exists cannot come back off-screen. An invalid
// desired rect is returned unchanged (callers treat it as "nothing saved").
QRect clampToVirtualDesktop(const QRect &desired, const QRect &available, const QSize &minimumSize);

} // namespace window_chrome_geometry
