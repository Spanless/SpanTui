#include "core/product_paths.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSettings>
#include <QTemporaryDir>
#include <QtTest>

using span::core::ProductPaths;

class ProductPathsTest final : public QObject {
    Q_OBJECT

private slots:
    void cleanup();
    void defaultsToLocalAppData();
    void portableMarkerKeepsDataNextToExecutable();
    void directoryNamedLikeMarkerIsNotPortable();
    void overrideWinsOverPortable();
    void everyDurableFileLivesUnderRoot();
    void movesLegacySettingsIntoSettingsIni();
    void keepsSettingsIniOverLeftoverLegacySettings();
    void movesLegacyRootIntoMissingRoot();
    void keepsLegacyParentThatStillHoldsFiles();
    void leavesLegacyRootWhenRootExists();
    void defaultRootMoveGoesFromAppDataToLocalAppData();
    void defaultRootMoveSkipsOverriddenRoot();

private:
    QByteArray m_appData = qgetenv("APPDATA");
    QByteArray m_localAppData = qgetenv("LOCALAPPDATA");

    static void writeFile(const QString &path, const QByteArray &contents = "x");
    static QByteArray readFile(const QString &path);
};

void ProductPathsTest::writeFile(const QString &path, const QByteArray &contents)
{
    QVERIFY(QDir().mkpath(QFileInfo(path).absolutePath()));
    QFile file(path);
    QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
    QCOMPARE(file.write(contents), contents.size());
}

QByteArray ProductPathsTest::readFile(const QString &path)
{
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
}

void ProductPathsTest::cleanup()
{
    ProductPaths::setRootOverride(QString());
    qputenv("APPDATA", m_appData);
    qputenv("LOCALAPPDATA", m_localAppData);
}

void ProductPathsTest::defaultsToLocalAppData()
{
    QTemporaryDir app;
    QVERIFY(app.isValid());
    QCOMPARE(ProductPaths::resolveRoot(QString(), app.path(), QStringLiteral("C:/Users/u/AppData/Local")),
             QStringLiteral("C:/Users/u/AppData/Local/SpanTui"));
}

void ProductPathsTest::portableMarkerKeepsDataNextToExecutable()
{
    QTemporaryDir app;
    QVERIFY(app.isValid());
    writeFile(QDir(app.path()).filePath(ProductPaths::portableMarkerName()));
    QCOMPARE(ProductPaths::resolveRoot(QString(), app.path(), QStringLiteral("C:/Local")),
             QDir(app.path()).filePath(QStringLiteral("data")));
}

void ProductPathsTest::directoryNamedLikeMarkerIsNotPortable()
{
    QTemporaryDir app;
    QVERIFY(app.isValid());
    QVERIFY(QDir(app.path()).mkdir(ProductPaths::portableMarkerName()));
    QCOMPARE(ProductPaths::resolveRoot(QString(), app.path(), QStringLiteral("C:/Local")),
             QStringLiteral("C:/Local/SpanTui"));
}

void ProductPathsTest::overrideWinsOverPortable()
{
    QTemporaryDir app;
    QVERIFY(app.isValid());
    writeFile(QDir(app.path()).filePath(ProductPaths::portableMarkerName()));
    QCOMPARE(ProductPaths::resolveRoot(QStringLiteral("C:/work/repo/.dev/data/../data"), app.path(),
                                       QStringLiteral("C:/Local")),
             QStringLiteral("C:/work/repo/.dev/data"));

    ProductPaths::setRootOverride(app.path());
    QCOMPARE(ProductPaths::root(), QDir::cleanPath(app.path()));
    QCOMPARE(ProductPaths::rootOverride(), app.path());
    QVERIFY(!ProductPaths::isPortable());
}

void ProductPathsTest::everyDurableFileLivesUnderRoot()
{
    ProductPaths::setRootOverride(QStringLiteral("C:/data"));

    QCOMPARE(ProductPaths::databasePath(), QStringLiteral("C:/data/span_tui.db"));
    QCOMPARE(ProductPaths::settingsPath(), QStringLiteral("C:/data/settings.ini"));
    QCOMPARE(ProductPaths::launchLogPath(), QStringLiteral("C:/data/last-launch.log"));
    QCOMPARE(ProductPaths::crashLogDirectory(), QStringLiteral("C:/data"));
    QCOMPARE(ProductPaths::logDirectory(), QStringLiteral("C:/data/logs"));
    QCOMPARE(ProductPaths::cacheDirectory(), QStringLiteral("C:/data/cache"));
    QCOMPARE(ProductPaths::pipelineCachePath(), QStringLiteral("C:/data/cache/product_paths_test.qtpipelinecache"));
    QCOMPARE(ProductPaths::isolatedRunsDirectory(), QStringLiteral("C:/data/isolated"));
    QCOMPARE(ProductPaths::hangDiagnosticDirectory(), QStringLiteral("C:/data/diagnostics/hang"));
    QCOMPARE(ProductPaths::graphicsStartMarkerPath(), QStringLiteral("C:/data/product_paths_test.graphics-start"));
}

void ProductPathsTest::movesLegacySettingsIntoSettingsIni()
{
    QTemporaryDir data;
    QVERIFY(data.isValid());
    ProductPaths::setRootOverride(data.path());
    const QString legacy = QDir(data.path()).filePath(QStringLiteral("Span/SpanTui.ini"));
    writeFile(legacy, QByteArrayLiteral("[localUpdate]\nsourcePath=C:/updates\n"));

    ProductPaths::installApplicationSettings();

    QVERIFY(!QFileInfo::exists(legacy));
    QVERIFY(!QFileInfo::exists(QDir(data.path()).filePath(QStringLiteral("Span"))));
    const QSettings settings(ProductPaths::settingsPath(), QSettings::IniFormat);
    QCOMPARE(settings.value(QStringLiteral("localUpdate/sourcePath")).toString(),
             QStringLiteral("C:/updates"));
}

void ProductPathsTest::keepsSettingsIniOverLeftoverLegacySettings()
{
    QTemporaryDir data;
    QVERIFY(data.isValid());
    ProductPaths::setRootOverride(data.path());
    writeFile(ProductPaths::settingsPath(), QByteArrayLiteral("[localUpdate]\nsourcePath=C:/updates\n"));
    writeFile(QDir(data.path()).filePath(QStringLiteral("Span/SpanTui.ini")),
              QByteArrayLiteral("[localUpdate]\nsourcePath=C:/stale\n"));

    ProductPaths::installApplicationSettings();

    const QSettings settings(ProductPaths::settingsPath(), QSettings::IniFormat);
    QCOMPARE(settings.value(QStringLiteral("localUpdate/sourcePath")).toString(), QStringLiteral("C:/updates"));
}

void ProductPathsTest::movesLegacyRootIntoMissingRoot()
{
    QTemporaryDir profile;
    QVERIFY(profile.isValid());
    const QString legacy = QDir(profile.path()).filePath(QStringLiteral("Roaming/Span/SpanTui"));
    const QString root = QDir(profile.path()).filePath(QStringLiteral("Local/SpanTui"));
    writeFile(QDir(legacy).filePath(QStringLiteral("span_tui.db")), QByteArrayLiteral("db"));

    QCOMPARE(ProductPaths::moveLegacyRoot(legacy, root), QString());

    QCOMPARE(readFile(QDir(root).filePath(QStringLiteral("span_tui.db"))), QByteArrayLiteral("db"));
    QVERIFY(!QFileInfo::exists(legacy));
    QVERIFY(!QFileInfo::exists(QDir(profile.path()).filePath(QStringLiteral("Roaming/Span"))));
}

void ProductPathsTest::keepsLegacyParentThatStillHoldsFiles()
{
    QTemporaryDir profile;
    QVERIFY(profile.isValid());
    const QString legacy = QDir(profile.path()).filePath(QStringLiteral("Roaming/Span/SpanTui"));
    const QString sibling = QDir(profile.path()).filePath(QStringLiteral("Roaming/Span/Other/keep.txt"));
    writeFile(QDir(legacy).filePath(QStringLiteral("span_tui.db")));
    writeFile(sibling);

    QCOMPARE(ProductPaths::moveLegacyRoot(legacy, QDir(profile.path()).filePath(QStringLiteral("Local/SpanTui"))),
             QString());

    QVERIFY(QFileInfo(sibling).isFile());
}

void ProductPathsTest::leavesLegacyRootWhenRootExists()
{
    QTemporaryDir profile;
    QVERIFY(profile.isValid());
    const QString legacy = QDir(profile.path()).filePath(QStringLiteral("Roaming/Span/SpanTui"));
    const QString root = QDir(profile.path()).filePath(QStringLiteral("Local/SpanTui"));
    writeFile(QDir(legacy).filePath(QStringLiteral("span_tui.db")), QByteArrayLiteral("old"));
    writeFile(QDir(root).filePath(QStringLiteral("span_tui.db")), QByteArrayLiteral("new"));

    QCOMPARE(ProductPaths::moveLegacyRoot(legacy, root), QString());

    QCOMPARE(readFile(QDir(root).filePath(QStringLiteral("span_tui.db"))), QByteArrayLiteral("new"));
    QCOMPARE(readFile(QDir(legacy).filePath(QStringLiteral("span_tui.db"))), QByteArrayLiteral("old"));
}

void ProductPathsTest::defaultRootMoveGoesFromAppDataToLocalAppData()
{
    QTemporaryDir profile;
    QVERIFY(profile.isValid());
    const QString roaming = QDir(profile.path()).filePath(QStringLiteral("Roaming"));
    const QString local = QDir(profile.path()).filePath(QStringLiteral("Local"));
    qputenv("APPDATA", QDir::toNativeSeparators(roaming).toLocal8Bit());
    qputenv("LOCALAPPDATA", QDir::toNativeSeparators(local).toLocal8Bit());
    writeFile(QDir(roaming).filePath(QStringLiteral("Span/SpanTui/settings.ini")), QByteArrayLiteral("ini"));

    QCOMPARE(ProductPaths::moveLegacyDefaultRoot(), QString());

    QCOMPARE(QDir::cleanPath(ProductPaths::root()), QDir::cleanPath(QDir(local).filePath(QStringLiteral("SpanTui"))));
    QCOMPARE(readFile(QDir(local).filePath(QStringLiteral("SpanTui/settings.ini"))), QByteArrayLiteral("ini"));
}

void ProductPathsTest::defaultRootMoveSkipsOverriddenRoot()
{
    QTemporaryDir data;
    QVERIFY(data.isValid());
    const QString root = QDir(data.path()).filePath(QStringLiteral("root"));
    ProductPaths::setRootOverride(root);

    QCOMPARE(ProductPaths::moveLegacyDefaultRoot(), QString());

    QVERIFY(!QFileInfo::exists(root));
}

QTEST_GUILESS_MAIN(ProductPathsTest)

#include "product_paths_test.moc"
