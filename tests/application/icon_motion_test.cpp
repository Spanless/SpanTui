#include "application/icon_motion.h"
#include "application/application_constants.h"

#include <QJsonDocument>
#include <QJsonObject>
#include <QTest>

#include <cmath>

namespace {

using span::application::kIconMotionSettleMs;

const QHash<QString, QString> kPaths{
    {QStringLiteral("bar"), QStringLiteral("M4 12L20 12")},
    {QStringLiteral("bar-to"), QStringLiteral("M6 12L20 12")},
    {QStringLiteral("dot"), QStringLiteral("M11 11L13 13")},
    {QStringLiteral("tri"), QStringLiteral("M4 12L12 4L20 12")},
    {QStringLiteral("arc"), QStringLiteral("M1 12A11 11 0 0 1 23 12Z")},
    {QStringLiteral("arc-to"), QStringLiteral("M3 12A9 9 0 0 1 21 12Z")},
};

const double kPi = std::acos(-1.0);

QJsonObject json(const char *text) {
    return QJsonDocument::fromJson(QByteArray(text)).object();
}

IconMotionSpec parseOrFail(const char *text) {
    IconMotionSpec spec;
    QString error;
    const bool ok = IconMotionSpec::parse(json(text), kPaths, &spec, &error);
    if (!ok) {
        qWarning("unexpected parse failure: %s", qPrintable(error));
    }
    return spec;
}

QPointF mapped(const IconMotionFrame &frame, QPointF point) {
    return frame.matrix.map(point);
}

bool near(double a, double b, double eps = 1e-3) {
    return std::abs(a - b) < eps;
}

// One part turning about the grid centre: hover rotates 90° (linear, wraps
// at 90), active tints it, press squashes it.
const char *kSpin = R"({
  "parts": [{"id": "bar", "path": "bar", "origin": [12, 12], "wrap": 90, "tint": "accent", "length": 16}],
  "hover": [{"part": "bar", "prop": "rotate", "dur": 100, "keys": [[0, 0], [1, 90, "l"]]}],
  "press": [{"part": "bar", "prop": "scale", "dur": 100, "keys": [[0, 0], [0.5, -0.5, "l"], [1, 0, "l"]]}],
  "active": [{"part": "bar", "prop": "tint", "dur": 100, "keys": [[0, 0], [1, 1, "l"]]},
             {"part": "bar", "prop": "draw", "dur": 100, "keys": [[0, -1], [1, 0, "l"]]}]
})";

} // namespace

class IconMotionTest final : public QObject {
    Q_OBJECT

private slots:
    void restIsIdentity();
    void hoverHoldsThenSettlesToWrappedRest();
    void pressIsOneShot();
    void activePoseIsStaticButNotRest();
    void activeSilencesHoverOnSharedProperties();
    void instantActiveLandsOnPose();
    void reducedMotionDropsHoverAndSnapsActive();
    void childrenInheritParentTransformAndOpacity();
    void morphInterpolatesPathNumbers();
    void morphKeepsArcFlagsSingleDigits();
    void rejectsInvalidSpecs_data();
    void rejectsInvalidSpecs();
};

void IconMotionTest::restIsIdentity() {
    const IconMotionSpec spec = parseOrFail(kSpin);
    IconMotionPlayer player(&spec);
    const auto frames = player.evaluate(0);
    QCOMPARE(frames.size(), 1);
    QVERIFY(frames[0].matrix.isIdentity());
    QCOMPARE(frames[0].tint, 0.0);
    QCOMPARE(frames[0].draw, 1.0);
    QVERIFY(player.atRest(0));
    QVERIFY(player.settled(0));
}

void IconMotionTest::hoverHoldsThenSettlesToWrappedRest() {
    const IconMotionSpec spec = parseOrFail(kSpin);
    IconMotionPlayer player(&spec);
    player.setHovered(true, 0);
    QVERIFY(!player.settled(50));

    // Halfway: the bar's right end (20,12) has turned 45° about (12,12).
    QPointF end = mapped(player.evaluate(50)[0], QPointF(20, 12));
    QVERIFY(near(end.x(), 12 + 8 * std::cos(kPi / 4)));
    QVERIFY(near(end.y(), 12 + 8 * std::sin(kPi / 4)));

    // Held at the final value while still hovered.
    player.evaluate(400);
    QVERIFY(player.settled(400));
    QVERIFY(!player.atRest(400));

    // Leaving settles to the nearest wrap multiple (90° looks like rest).
    player.setHovered(false, 400);
    QVERIFY(!player.settled(400 + 10));
    player.evaluate(400 + kIconMotionSettleMs + 1);
    QVERIFY(player.atRest(400 + kIconMotionSettleMs + 1));
    QVERIFY(player.evaluate(1000)[0].matrix.isIdentity());
}

void IconMotionTest::pressIsOneShot() {
    const IconMotionSpec spec = parseOrFail(kSpin);
    IconMotionPlayer player(&spec);
    player.press(0);
    QVERIFY(!player.atRest(10));
    const QPointF squashed = mapped(player.evaluate(50)[0], QPointF(20, 12));
    QVERIFY(near(squashed.x(), 16));
    player.evaluate(101);
    QVERIFY(player.settled(101));
    QVERIFY(player.atRest(101));
}

void IconMotionTest::activePoseIsStaticButNotRest() {
    const IconMotionSpec spec = parseOrFail(kSpin);
    IconMotionPlayer player(&spec);
    player.setActive(true, 0);
    QVERIFY(near(player.evaluate(50)[0].draw, 0.5));
    const auto pose = player.evaluate(150)[0];
    QCOMPARE(pose.tint, 1.0);
    QCOMPARE(pose.draw, 1.0);
    // No clock is needed to hold the pose, yet it differs from the glyph.
    QVERIFY(player.settled(150));
    QVERIFY(!player.atRest(150));

    player.setActive(false, 150);
    QVERIFY(!player.settled(160));
    QVERIFY(player.evaluate(150 + kIconMotionSettleMs + 1)[0].tint == 0.0);
    QVERIFY(player.atRest(150 + kIconMotionSettleMs + 1));
}

void IconMotionTest::activeSilencesHoverOnSharedProperties() {
    const IconMotionSpec spec = parseOrFail(R"({
      "parts": [{"id": "bar", "path": "bar", "origin": [12, 12], "tint": "accent"}],
      "hover": [{"part": "bar", "prop": "tint", "dur": 10, "keys": [[0, 0], [1, 0.4, "l"]]},
                {"part": "bar", "prop": "tx", "dur": 10, "keys": [[0, 0], [1, 2, "l"]]}],
      "active": [{"part": "bar", "prop": "tint", "dur": 10, "keys": [[0, 0], [1, 1, "l"]]}]
    })");
    IconMotionPlayer player(&spec);
    player.setActive(true, 0);
    player.setHovered(true, 0);
    const auto frame = player.evaluate(100)[0];
    // Tint belongs to the active pose; hover still owns translation.
    QCOMPARE(frame.tint, 1.0);
    QVERIFY(near(mapped(frame, QPointF(12, 12)).x(), 14));
}

void IconMotionTest::instantActiveLandsOnPose() {
    const IconMotionSpec spec = parseOrFail(kSpin);
    IconMotionPlayer player(&spec);
    player.setActive(true, 0, true);
    QCOMPARE(player.evaluate(0)[0].tint, 1.0);
    QVERIFY(player.settled(0));
}

void IconMotionTest::reducedMotionDropsHoverAndSnapsActive() {
    const IconMotionSpec spec = parseOrFail(kSpin);
    IconMotionPlayer player(&spec);
    player.setReducedMotion(true);
    player.setHovered(true, 0);
    player.press(0);
    QVERIFY(player.atRest(0));
    QVERIFY(player.evaluate(0)[0].matrix.isIdentity());

    player.setActive(true, 0);
    QCOMPARE(player.evaluate(0)[0].tint, 1.0);
    QVERIFY(player.settled(0));
    player.setActive(false, 0);
    QCOMPARE(player.evaluate(0)[0].tint, 0.0);
    QVERIFY(player.atRest(0));
}

void IconMotionTest::childrenInheritParentTransformAndOpacity() {
    const IconMotionSpec spec = parseOrFail(R"({
      "parts": [{"id": "group", "origin": [12, 12]},
                {"id": "dot", "path": "dot", "origin": [12, 12], "parent": "group", "rest": {"opacity": 0.5}}],
      "hover": [{"part": "group", "prop": "tx", "dur": 10, "keys": [[0, 0], [1, 3, "l"]]},
                {"part": "group", "prop": "opacity", "dur": 10, "keys": [[0, 0], [1, -0.5, "l"]]},
                {"part": "dot", "prop": "ty", "dur": 10, "keys": [[0, 0], [1, 2, "l"]]}]
    })");
    IconMotionPlayer player(&spec);
    player.setHovered(true, 0);
    const auto frames = player.evaluate(100);
    const QPointF p = mapped(frames[1], QPointF(12, 12));
    QVERIFY(near(p.x(), 15));
    QVERIFY(near(p.y(), 14));
    QVERIFY(near(frames[1].opacity, 0.25));
}

void IconMotionTest::morphInterpolatesPathNumbers() {
    const IconMotionSpec spec = parseOrFail(R"({
      "parts": [{"id": "bar", "path": "bar", "morph": "bar-to", "origin": [12, 12]}],
      "hover": [{"part": "bar", "prop": "morph", "dur": 10, "keys": [[0, 0], [1, 1, "l"]]}]
    })");
    QVERIFY(spec.parts[0].morphs());
    QCOMPARE(spec.parts[0].morphPath(0.5), QStringLiteral("M5 12L20 12"));
    QCOMPARE(spec.parts[0].morphPath(0.123456), QStringLiteral("M4.247 12L20 12"));
    IconMotionPlayer player(&spec);
    player.setHovered(true, 0);
    QCOMPARE(player.evaluate(100)[0].morph, 1.0);
}

void IconMotionTest::morphKeepsArcFlagsSingleDigits()
{
    // Qt reads an arc flag as one digit; "0.000 1.000" breaks the path.
    const IconMotionSpec spec = parseOrFail(R"({
      "parts": [{"id": "disc", "path": "arc", "morph": "arc-to", "origin": [12, 12], "solid": true}]
    })");
    QCOMPARE(spec.parts[0].morphPath(0), QStringLiteral("M1 12A11 11 0 0 1 23 12Z"));
    QCOMPARE(spec.parts[0].morphPath(0.5), QStringLiteral("M2 12A10 10 0 0 1 22 12Z"));
    // Overshooting easings ("back") still keep the shared flags exact.
    QCOMPARE(spec.parts[0].morphPath(-0.25), QStringLiteral("M0.5 12A11.5 11.5 0 0 1 23.5 12Z"));
}

void IconMotionTest::rejectsInvalidSpecs_data() {
    QTest::addColumn<QByteArray>("text");
    QTest::newRow("active loops") << QByteArray(R"({"parts": [{"id": "bar", "path": "bar", "origin": [12, 12]}],
        "active": [{"part": "bar", "prop": "tx", "dur": 10, "loop": true, "keys": [[0, 0], [1, 1]]}]})");
    QTest::newRow("press ends away from rest") << QByteArray(R"({"parts": [{"id": "bar", "path": "bar", "origin": [12, 12]}],
        "press": [{"part": "bar", "prop": "tx", "dur": 10, "keys": [[0, 0], [1, 1]]}]})");
    QTest::newRow("unknown part") << QByteArray(R"({"parts": [{"id": "bar", "path": "bar", "origin": [12, 12]}],
        "hover": [{"part": "nope", "prop": "tx", "dur": 10, "keys": [[0, 0]]}]})");
    QTest::newRow("unknown path") << QByteArray(R"({"parts": [{"id": "bar", "path": "missing", "origin": [12, 12]}]})");
    QTest::newRow("draw without length") << QByteArray(R"({"parts": [{"id": "bar", "path": "bar", "origin": [12, 12]}],
        "hover": [{"part": "bar", "prop": "draw", "dur": 10, "keys": [[0, -1], [1, 0]]}]})");
    QTest::newRow("parent after child") << QByteArray(R"({"parts": [{"id": "dot", "path": "dot", "origin": [12, 12], "parent": "g"},
        {"id": "g", "origin": [12, 12]}]})");
    QTest::newRow("keys out of order") << QByteArray(R"({"parts": [{"id": "bar", "path": "bar", "origin": [12, 12]}],
        "hover": [{"part": "bar", "prop": "tx", "dur": 10, "keys": [[0.5, 0], [0.2, 1]]}]})");
    QTest::newRow("unknown tint") << QByteArray(R"({"parts": [{"id": "bar", "path": "bar", "origin": [12, 12], "tint": "pink"}]})");
    QTest::newRow("morph structure differs") << QByteArray(R"({"parts": [{"id": "bar", "path": "bar", "morph": "tri", "origin": [12, 12]}]})");
}

void IconMotionTest::rejectsInvalidSpecs() {
    QFETCH(QByteArray, text);
    IconMotionSpec spec;
    QString error;
    QVERIFY(!IconMotionSpec::parse(QJsonDocument::fromJson(text).object(), kPaths, &spec, &error));
    QVERIFY(!error.isEmpty());
}

QTEST_GUILESS_MAIN(IconMotionTest)
#include "icon_motion_test.moc"
