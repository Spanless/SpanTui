#include "pixel_snap.h"

#include <QMatrix4x4>
#include <QQuickWindow>
#include <QTransform>

#include <cmath>

namespace {

constexpr qreal kEpsilon = 1e-4;

qreal snapped(qreal value, qreal dpr) {
    return std::round(value * dpr) / dpr;
}

} // namespace

PixelSnap::PixelSnap(QObject *parent)
    : QQuickTransform(parent) {}

void PixelSnap::setItem(QQuickItem *item) {
    if (m_item == item)
        return;
    QObject::disconnect(m_windowConnection);
    m_item = item;
    m_windowConnection = item
        ? connect(item, &QQuickItem::windowChanged, this, &PixelSnap::attachWindow)
        : QMetaObject::Connection();
    attachWindow(item ? item->window() : nullptr);
    emit itemChanged();
}

void PixelSnap::attachWindow(QQuickWindow *window) {
    if (m_window == window)
        return;
    QObject::disconnect(m_frameConnection);
    m_window = window;
    // afterAnimating runs on the GUI thread after polish and before sync, so
    // an offset set here lands in the same frame as the move that caused it.
    m_frameConnection = window
        ? connect(window, &QQuickWindow::afterAnimating, this, &PixelSnap::realign)
        : QMetaObject::Connection();
    realign();
}

void PixelSnap::realign() {
    QPointF next;
    QQuickItem *parent = m_item ? m_item->parentItem() : nullptr;
    if (m_window && parent) {
        if (!m_item->isVisible())
            return;
        bool ok = false;
        const QTransform toScene = parent->itemTransform(nullptr, &ok);
        // Only translation and scale keep pixel edges axis-aligned; under a
        // rotation (or a bad mapping) there is no pixel grid to land on.
        if (ok && toScene.type() <= QTransform::TxScale
            && toScene.m11() > 0 && toScene.m22() > 0) {
            const qreal dpr = m_window->effectiveDevicePixelRatio();
            const QPointF origin = toScene.map(m_item->position());
            next = QPointF((snapped(origin.x(), dpr) - origin.x()) / toScene.m11(),
                           (snapped(origin.y(), dpr) - origin.y()) / toScene.m22());
        }
    }
    if (std::abs(next.x() - m_offset.x()) < kEpsilon
        && std::abs(next.y() - m_offset.y()) < kEpsilon)
        return;
    m_offset = next;
    update();
}

void PixelSnap::applyTo(QMatrix4x4 *matrix) const {
    if (!m_offset.isNull())
        matrix->translate(float(m_offset.x()), float(m_offset.y()));
}
