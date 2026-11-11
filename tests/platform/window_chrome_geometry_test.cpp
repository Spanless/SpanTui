#include "platform/windows/window_chrome_geometry.h"

#include <QtTest>

using window_chrome_geometry::HitRegion;
using window_chrome_geometry::clampToVirtualDesktop;
using window_chrome_geometry::hitTest;

namespace {

constexpr int kBand = 8;
constexpr int kTitleBar = 32;
const QSize kClient(1000, 700);

HitRegion runHitTest(const QPoint &pos,
                     bool maximized = false,
                     const QRect &maxButton = QRect(),
                     const QVector<QRect> &interactive = {}) {
    return hitTest(pos, kClient, kBand, kTitleBar, maxButton, interactive, maximized);
}

} // namespace

class WindowChromeGeometryTest : public QObject {
    Q_OBJECT

private slots:
    void hitsResizeEdges() {
        QCOMPARE(runHitTest(QPoint(2, 300)), HitRegion::Left);
        QCOMPARE(runHitTest(QPoint(997, 300)), HitRegion::Right);
        QCOMPARE(runHitTest(QPoint(500, 2)), HitRegion::Top);
        QCOMPARE(runHitTest(QPoint(500, 697)), HitRegion::Bottom);
    }

    void hitsResizeCorners() {
        QCOMPARE(runHitTest(QPoint(2, 2)), HitRegion::TopLeft);
        QCOMPARE(runHitTest(QPoint(997, 2)), HitRegion::TopRight);
        QCOMPARE(runHitTest(QPoint(2, 697)), HitRegion::BottomLeft);
        QCOMPARE(runHitTest(QPoint(997, 697)), HitRegion::BottomRight);
    }

    void titleBarBandIsCaption() {
        QCOMPARE(runHitTest(QPoint(500, 24)), HitRegion::Caption);
        QCOMPARE(runHitTest(QPoint(500, kTitleBar - 1)), HitRegion::Caption);
    }

    void belowTitleBarIsClient() {
        QCOMPARE(runHitTest(QPoint(500, kTitleBar)), HitRegion::Client);
        QCOMPARE(runHitTest(QPoint(500, 350)), HitRegion::Client);
    }

    void maxButtonWinsInsideTitleBar() {
        const QRect maxButton(860, 0, 46, kTitleBar);
        QCOMPARE(runHitTest(QPoint(880, 20), false, maxButton), HitRegion::MaxButton);
        QCOMPARE(runHitTest(QPoint(850, 20), false, maxButton), HitRegion::Caption);
    }

    void interactiveItemsPunchThroughCaption() {
        const QVector<QRect> interactive{QRect(200, 9, 30, 30)};
        QCOMPARE(runHitTest(QPoint(210, 20), false, QRect(), interactive), HitRegion::Client);
        QCOMPARE(runHitTest(QPoint(300, 20), false, QRect(), interactive), HitRegion::Caption);
    }

    void maximizedWindowHasNoResizeEdges() {
        QCOMPARE(runHitTest(QPoint(2, 300), true), HitRegion::Client);
        QCOMPARE(runHitTest(QPoint(500, 2), true), HitRegion::Caption);
        QCOMPARE(runHitTest(QPoint(2, 2), true), HitRegion::Caption);
    }

    void outsideClientIsNowhere() {
        QCOMPARE(runHitTest(QPoint(-5, 20)), HitRegion::Nowhere);
        QCOMPARE(runHitTest(QPoint(500, 800)), HitRegion::Nowhere);
    }

    void clampKeepsValidRectUntouched() {
        const QRect available(0, 0, 2560, 1440);
        const QRect desired(100, 120, 1320, 820);
        QCOMPARE(clampToVirtualDesktop(desired, available, QSize(980, 640)), desired);
    }

    void clampPullsOffscreenRectBack() {
        const QRect available(0, 0, 1920, 1080);
        const QRect clamped = clampToVirtualDesktop(QRect(3000, 2000, 1320, 820), available, QSize(980, 640));
        QCOMPARE(clamped, QRect(600, 260, 1320, 820));
    }

    void clampShrinksOversizedRect() {
        const QRect available(0, 0, 1280, 720);
        const QRect clamped = clampToVirtualDesktop(QRect(0, 0, 1920, 1080), available, QSize(980, 640));
        QCOMPARE(clamped, QRect(0, 0, 1280, 720));
    }

    void clampHandlesNegativeVirtualDesktopOrigin() {
        // A monitor to the left of the primary puts the desktop origin at
        // negative coordinates; positions there must survive clamping.
        const QRect available(-1920, 0, 3840, 1080);
        const QRect desired(-1800, 100, 1320, 820);
        QCOMPARE(clampToVirtualDesktop(desired, available, QSize(980, 640)), desired);
    }

    void clampPassesInvalidRectThrough() {
        const QRect available(0, 0, 1920, 1080);
        QVERIFY(!clampToVirtualDesktop(QRect(), available, QSize(980, 640)).isValid());
    }
};

QTEST_APPLESS_MAIN(WindowChromeGeometryTest)

#include "window_chrome_geometry_test.moc"
