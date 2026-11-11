#include "application/pixel_snap.h"

#include <QQuickItem>
#include <QQuickWindow>
#include <QtTest>

// Window is never shown; its device pixel ratio is the offscreen screen's (1).
class PixelSnapTest final : public QObject {
    Q_OBJECT

private slots:
    void landsOriginOnPixel();
    void followsAncestorMoves();
    void scaledAncestorShiftsInLocalUnits();
    void rotatedAncestorDisablesSnap();
    void noWindowNoOffset();
};

namespace {

struct Scene {
    QQuickWindow window;
    QQuickItem parent;
    QQuickItem child;
    PixelSnap snap;

    Scene() {
        parent.setParentItem(window.contentItem());
        child.setParentItem(&parent);
        snap.appendToItem(&child);
        snap.setItem(&child);
    }

    QPointF childOrigin() const { return child.mapToScene(QPointF()); }
};

} // namespace

void PixelSnapTest::landsOriginOnPixel() {
    Scene scene;
    scene.parent.setPosition(QPointF(10.25, 3.5));
    scene.snap.realign();

    QCOMPARE(scene.snap.offset(), QPointF(-0.25, 0.5));
    QCOMPARE(scene.childOrigin(), QPointF(10, 4));
}

void PixelSnapTest::followsAncestorMoves() {
    Scene scene;
    scene.parent.setPosition(QPointF(2, 2));
    scene.snap.realign();
    QCOMPARE(scene.snap.offset(), QPointF());

    scene.parent.setX(7.6);
    scene.snap.realign();
    QCOMPARE(scene.childOrigin(), QPointF(8, 2));
}

void PixelSnapTest::scaledAncestorShiftsInLocalUnits() {
    Scene scene;
    scene.parent.setTransformOrigin(QQuickItem::TopLeft);
    scene.parent.setScale(2);
    scene.parent.setPosition(QPointF(10, 0));
    scene.child.setPosition(QPointF(0.3, 0));
    scene.snap.realign();

    QCOMPARE(scene.snap.offset().x(), 0.2);
    QCOMPARE(scene.childOrigin(), QPointF(11, 0));
}

void PixelSnapTest::rotatedAncestorDisablesSnap() {
    Scene scene;
    scene.parent.setPosition(QPointF(10.25, 3.5));
    scene.snap.realign();
    QVERIFY(!scene.snap.offset().isNull());

    scene.parent.setRotation(30);
    scene.snap.realign();
    QCOMPARE(scene.snap.offset(), QPointF());
}

void PixelSnapTest::noWindowNoOffset() {
    QQuickItem parent;
    QQuickItem child(&parent);
    child.setParentItem(&parent);
    parent.setPosition(QPointF(10.25, 3.5));
    PixelSnap snap;
    snap.appendToItem(&child);
    snap.setItem(&child);
    snap.realign();

    QCOMPARE(snap.offset(), QPointF());
}

QTEST_MAIN(PixelSnapTest)
#include "pixel_snap_test.moc"
