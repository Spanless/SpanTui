#include "application/icon_motion.h"
#include "application/ui_icon_catalog.h"

#include <QAnimationDriver>
#include <QDirIterator>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QMatrix4x4>
#include <QRegularExpression>
#include <QScopeGuard>
#include <QSet>
#include <QSvgRenderer>
#include <QTest>
#include <QXmlStreamReader>
#include <QtMath>

#include <algorithm>
#include <cmath>

namespace {

// Moves animation time a fixed step per tick whatever the real interval, as
// Qt Quick's default driver adds one vsync per frame.
class SteppingDriver final : public QAnimationDriver {
public:
    void step(qint64 ms)
    {
        m_time += ms;
        advance();
    }
    qint64 elapsed() const override { return m_time; }

private:
    qint64 m_time = 0;
};

// Rotation about z of a part's frame matrix, in degrees.
double rotationOf(const QVariant &frame)
{
    const QMatrix4x4 m = frame.toMap().value(QStringLiteral("matrix")).value<QMatrix4x4>();
    return qRadiansToDegrees(std::atan2(m(1, 0), m(0, 0)));
}

QStringList filesUnder(const QString &directory, const QString &pattern)
{
    QStringList paths;
    QDirIterator iterator(directory, QStringList{pattern}, QDir::Files, QDirIterator::Subdirectories);
    while (iterator.hasNext()) paths.push_back(iterator.next());
    paths.sort();
    return paths;
}

QStringList unrenderableSvgs(const QStringList &paths)
{
    QStringList invalid;
    for (const QString &path : paths) {
        if (!QSvgRenderer(path).isValid()) invalid.push_back(path);
    }
    return invalid;
}

// Of `names`, those the catalog has no path for, sorted.
template <typename Names> QStringList unresolvedNames(const UiIconCatalog &catalog, const Names &names)
{
    QStringList missing;
    for (const QString &name : names) {
        if (catalog.path(name).isEmpty()) missing.push_back(name);
    }
    missing.sort();
    return missing;
}

QSet<QString> qmlIconReferences(const QString &qmlRoot)
{
    const QRegularExpression quoted(QStringLiteral("(?:iconName|\\bicon):\\s*\"([a-z0-9-]+)\""));
    QSet<QString> referenced;
    for (const QString &path : filesUnder(qmlRoot, QStringLiteral("*.qml"))) {
        QFile file(path);
        if (!file.open(QIODevice::ReadOnly | QIODevice::Text))
            referenced.insert(QStringLiteral("<unreadable %1>").arg(path));
        auto match = quoted.globalMatch(QString::fromUtf8(file.readAll()));
        while (match.hasNext()) referenced.insert(match.next().captured(1));
    }
    return referenced;
}

// Every non-empty icon name file-icons.json maps a file, folder, extension or
// file name to.
QSet<QString> fileIconNames(const QJsonObject &root)
{
    QSet<QString> names;
    const auto take = [&names](const QJsonValue &value) {
        if (!value.toString().isEmpty()) names.insert(value.toString());
    };
    for (const char *key : {"file", "folder", "folderExpanded", "rootFolder", "rootFolderExpanded"})
        take(root.value(QLatin1String(key)));
    for (const char *key : {"fileExtensions", "fileNames"}) {
        const QJsonObject map = root.value(QLatin1String(key)).toObject();
        for (auto it = map.begin(); it != map.end(); ++it) take(it.value());
    }
    return names;
}

// Icon names that have a *.motion.json beside their SVG, sorted.
QStringList motionFileNames(const QString &iconRoot)
{
    QStringList names;
    for (const QString &path : filesUnder(iconRoot, QStringLiteral("*.motion.json")))
        names.push_back(QFileInfo(path).fileName().chopped(QStringLiteral(".motion.json").size()));
    names.sort();
    return names;
}

QStringList animatedNames(const UiIconCatalog &catalog, const QStringList &names)
{
    QStringList animated;
    for (const QString &name : names) {
        if (catalog.animated(name)) animated.push_back(name);
    }
    return animated;
}

// The space-joined `d` of every <path> outside <defs> in `svgPath`, and the
// `d` of each one inside.
struct GlyphPaths {
    QStringList visible;
    QStringList hidden;
};

GlyphPaths glyphPaths(const QString &svgPath)
{
    GlyphPaths paths;
    QFile file(svgPath);
    if (!file.open(QIODevice::ReadOnly)) return paths;
    QXmlStreamReader xml(&file);
    int defs = 0;
    while (!xml.atEnd()) {
        const auto token = xml.readNext();
        if (xml.name() == QLatin1String("defs")) {
            defs += token == QXmlStreamReader::StartElement ? 1 : token == QXmlStreamReader::EndElement ? -1 : 0;
        } else if (token == QXmlStreamReader::StartElement && xml.name() == QLatin1String("path")) {
            (defs > 0 ? paths.hidden : paths.visible).push_back(xml.attributes().value(QLatin1String("d")).toString());
        }
    }
    return paths;
}

// Icons whose static glyph is not exactly their visible paths, or that carry
// a <defs>-only path (a motion source) into it.
QStringList staticGlyphMismatches(const UiIconCatalog &catalog, const QStringList &svgPaths)
{
    QStringList mismatched;
    for (const QString &svgPath : svgPaths) {
        const QString name = QFileInfo(svgPath).completeBaseName();
        const GlyphPaths paths = glyphPaths(svgPath);
        const QString glyph = catalog.path(name);
        bool leaked = false;
        for (const QString &d : paths.hidden) leaked = leaked || (!paths.visible.contains(d) && glyph.contains(d));
        if (glyph != paths.visible.join(QLatin1Char(' ')) || leaked) mismatched.push_back(name);
    }
    return mismatched;
}

} // namespace

class UiIconCatalogTest final : public QObject {
    Q_OBJECT

private slots:
    void initTestCase();
    void loadsEverySvgUnderAssets();
    void unknownNameIsEmpty();
    void sourceStemsAreUnique();
    void qmlReferencesResolve();
    void playIsFilledAndPlusIsStroked();
    void fileIconAssociationsResolve();
    void motionFilesLoad();
    void defsStayOutOfStaticGlyph();
    void motionClockFollowsWallTime();

private:
    static QStringList sourceSvgPaths();
    static QStringList sourceNames();

    UiIconCatalog m_catalog;
};

QStringList UiIconCatalogTest::sourceSvgPaths()
{
    return filesUnder(QStringLiteral(SPANTUI_SOURCE_DIR "/assets/icons"), QStringLiteral("*.svg"));
}

QStringList UiIconCatalogTest::sourceNames()
{
    QStringList names;
    for (const QString &path : sourceSvgPaths()) {
        names.push_back(QFileInfo(path).completeBaseName());
    }
    names.sort();
    return names;
}

void UiIconCatalogTest::initTestCase()
{
    QVERIFY(m_catalog.ready());
}

void UiIconCatalogTest::loadsEverySvgUnderAssets()
{
    const QStringList disk = sourceNames();
    QVERIFY(disk.size() >= 70);
    QCOMPARE(m_catalog.names(), disk);
    QCOMPARE(unrenderableSvgs(sourceSvgPaths()), QStringList{});
    QCOMPARE(unresolvedNames(m_catalog, disk), QStringList{});
}

void UiIconCatalogTest::unknownNameIsEmpty()
{
    QVERIFY(m_catalog.path(QStringLiteral("not-an-icon")).isEmpty());
    QVERIFY(m_catalog.path(QString()).isEmpty());
}

void UiIconCatalogTest::sourceStemsAreUnique()
{
    const QStringList names = sourceNames();
    QCOMPARE(QSet<QString>(names.begin(), names.end()).size(), names.size());
}

void UiIconCatalogTest::qmlReferencesResolve()
{
    const QSet<QString> referenced = qmlIconReferences(QStringLiteral(SPANTUI_SOURCE_DIR "/qml"));
    QVERIFY(referenced.contains(QStringLiteral("settings")));
    QVERIFY(referenced.contains(QStringLiteral("history")));
    QCOMPARE(unresolvedNames(m_catalog, referenced), QStringList{});
}

void UiIconCatalogTest::playIsFilledAndPlusIsStroked()
{
    QVERIFY(m_catalog.filled(QStringLiteral("play")));
    QVERIFY(m_catalog.filled(QStringLiteral("star-filled")));
    QVERIFY(m_catalog.filled(QStringLiteral("tree-chevron")));
    QVERIFY(!m_catalog.filled(QStringLiteral("plus")));
    QVERIFY(!m_catalog.filled(QStringLiteral("star")));
    QVERIFY(!m_catalog.filled(QStringLiteral("not-an-icon")));
}

void UiIconCatalogTest::fileIconAssociationsResolve()
{
    QFile file(QStringLiteral(SPANTUI_SOURCE_DIR "/assets/icons/file-icons.json"));
    QVERIFY(file.open(QIODevice::ReadOnly));
    const QJsonDocument doc = QJsonDocument::fromJson(file.readAll());
    QVERIFY(doc.isObject());
    const QSet<QString> names = fileIconNames(doc.object());
    QVERIFY(names.contains(QStringLiteral("file")));
    QVERIFY(names.contains(QStringLiteral("type-cpp")));
    QCOMPARE(unresolvedNames(m_catalog, names), QStringList{});
}

void UiIconCatalogTest::motionFilesLoad()
{
    const QStringList motionNames = motionFileNames(QStringLiteral(SPANTUI_SOURCE_DIR "/assets/icons"));
    QVERIFY(motionNames.contains(QStringLiteral("bell")));
    QCOMPARE(animatedNames(m_catalog, sourceNames()), motionNames);
    const IconMotionSpec *bell = m_catalog.motion(QStringLiteral("bell"));
    QVERIFY(bell);
    QVERIFY(!bell->layers[IconMotionSpec::Hover].isEmpty());
    QVERIFY(!bell->layers[IconMotionSpec::Active].isEmpty());
    QVERIFY(!m_catalog.animated(QStringLiteral("type-cpp")));
    QVERIFY(!m_catalog.motion(QStringLiteral("not-an-icon")));

    // The QML-facing player: parts appear once the component completes,
    // hover moves it off rest, its clock brings it back, and an unknown name
    // leaves nothing to draw.
    IconMotion icon;
    icon.classBegin();
    icon.setCatalog(&m_catalog);
    icon.setName(QStringLiteral("bell"));
    QVERIFY(icon.parts().isEmpty());
    icon.componentComplete();
    QCOMPARE(icon.parts().size(), bell->parts.size());
    QCOMPARE(icon.frame().size(), bell->parts.size());
    QVERIFY(!icon.engaged());
    icon.setHovered(true);
    QVERIFY(icon.engaged());
    icon.press();
    icon.setHovered(false);
    QTRY_VERIFY_WITH_TIMEOUT(!icon.engaged(), 5000);
    // Reduced motion snaps to the held active pose instead of animating to it.
    icon.setReduceMotion(true);
    icon.setActive(true);
    QVERIFY(icon.engaged());
    icon.setName(QStringLiteral("not-an-icon"));
    QVERIFY(icon.parts().isEmpty());
    QVERIFY(!icon.engaged());
}

void UiIconCatalogTest::defsStayOutOfStaticGlyph()
{
    QCOMPARE(staticGlyphMismatches(m_catalog, sourceSvgPaths()), QStringList{});
}

void UiIconCatalogTest::motionClockFollowsWallTime()
{
    // The loading orbit turns once per 1100 ms. A driver claiming 825 ms in
    // three quick ticks would put it at -90°; timed by the wall clock it
    // stays where real time says.
    SteppingDriver driver;
    driver.install();
    const auto restore = qScopeGuard([&driver] { driver.uninstall(); });

    IconMotion icon;
    icon.classBegin();
    icon.setCatalog(&m_catalog);
    icon.setName(QStringLiteral("loading"));
    icon.componentComplete();
    QCOMPARE(icon.parts().size(), m_catalog.motion(QStringLiteral("loading"))->parts.size());

    QElapsedTimer wall;
    wall.start();
    icon.setHovered(true);
    QTRY_VERIFY_WITH_TIMEOUT(driver.isRunning(), 5000);
    driver.step(275);
    driver.step(275);
    driver.step(275);
    const double expected = 360.0 * (double(wall.nsecsElapsed()) / 1e6) / 1100.0;
    const double drift = std::remainder(rotationOf(icon.frame().at(1)) - expected, 360.0);
    QVERIFY2(std::abs(drift) < 5, qPrintable(QStringLiteral("drift %1°").arg(drift)));
}

QTEST_MAIN(UiIconCatalogTest)
#include "ui_icon_catalog_test.moc"
