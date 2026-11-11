#pragma once

#include <QMetaObject>
#include <QPointF>
#include <QPointer>
#include <QQuickItem>

class QQuickWindow;

// Sub-pixel translation that lands `item`'s origin on a physical pixel, so
// vector content (Icon shapes) gets one sharp coverage edge instead of two
// half-covered columns at fractional scale factors (125 %, 150 %, ...) or
// after centering. Listed last in `item`'s transform list, so the shift is in
// parent coordinates. Realigned every frame before the scene graph syncs,
// which also catches moves of any ancestor; rotated ancestors disable it.
// Rules: docs/ui/pixel-boundaries.md.
class PixelSnap : public QQuickTransform {
    Q_OBJECT
    Q_PROPERTY(QQuickItem *item READ item WRITE setItem NOTIFY itemChanged FINAL)

public:
    explicit PixelSnap(QObject *parent = nullptr);

    QQuickItem *item() const { return m_item; }
    void setItem(QQuickItem *item);
    QPointF offset() const { return m_offset; }

    void applyTo(QMatrix4x4 *matrix) const override;

    // Recomputes the offset now; the frame hook calls it, tests call it.
    void realign();

signals:
    void itemChanged();

private:
    void attachWindow(QQuickWindow *window);

    QPointer<QQuickItem> m_item;
    QPointer<QQuickWindow> m_window;
    QMetaObject::Connection m_windowConnection;
    QMetaObject::Connection m_frameConnection;
    QPointF m_offset;
};
