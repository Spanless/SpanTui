#include "platform/windows/window_chrome_geometry.h"

#include <algorithm>

namespace window_chrome_geometry {

HitRegion hitTest(const QPoint &pos,
                  const QSize &clientSize,
                  int resizeBand,
                  int titleBarHeight,
                  const QRect &maxButtonRect,
                  const QVector<QRect> &interactiveRects,
                  bool maximized) {
    // Maximized WS_THICKFRAME windows overhang the work area; hits in that
    // strip (e.g. over the taskbar) must not claim HTCLIENT or the bar dies.
    if (pos.x() < 0 || pos.y() < 0 || pos.x() >= clientSize.width() || pos.y() >= clientSize.height()) {
        return HitRegion::Nowhere;
    }

    if (!maximized) {
        const bool onLeft = pos.x() < resizeBand;
        const bool onRight = pos.x() >= clientSize.width() - resizeBand;
        const bool onTop = pos.y() < resizeBand;
        const bool onBottom = pos.y() >= clientSize.height() - resizeBand;

        if (onTop && onLeft) return HitRegion::TopLeft;
        if (onTop && onRight) return HitRegion::TopRight;
        if (onBottom && onLeft) return HitRegion::BottomLeft;
        if (onBottom && onRight) return HitRegion::BottomRight;
        if (onLeft) return HitRegion::Left;
        if (onRight) return HitRegion::Right;
        if (onTop) return HitRegion::Top;
        if (onBottom) return HitRegion::Bottom;
    }

    if (pos.y() < titleBarHeight) {
        if (maxButtonRect.isValid() && maxButtonRect.contains(pos)) {
            return HitRegion::MaxButton;
        }
        for (const QRect &rect : interactiveRects) {
            if (rect.isValid() && rect.contains(pos)) {
                return HitRegion::Client;
            }
        }
        return HitRegion::Caption;
    }

    return HitRegion::Client;
}

QRect clampToVirtualDesktop(const QRect &desired, const QRect &available, const QSize &minimumSize) {
    if (!desired.isValid() || !available.isValid()) {
        return desired;
    }

    const int width = std::clamp(desired.width(), std::min(minimumSize.width(), available.width()), available.width());
    const int height = std::clamp(desired.height(), std::min(minimumSize.height(), available.height()), available.height());
    const int x = std::clamp(desired.x(), available.left(), available.left() + available.width() - width);
    const int y = std::clamp(desired.y(), available.top(), available.top() + available.height() - height);
    return QRect(x, y, width, height);
}

} // namespace window_chrome_geometry
