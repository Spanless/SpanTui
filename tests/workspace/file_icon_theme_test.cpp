#include "workspace/file_icon_theme.h"

#include <QTest>

class FileIconThemeTest final : public QObject {
    Q_OBJECT

private slots:
    void initTestCase();
    void mapsLanguageFilesToTypeIcons();
    void mapsExactFileNamesAheadOfExtensions();
    void mapsDirectoriesToFolderGlyphs();
    void emptyNameFallsBackToFile();
    void compoundExtensionPrefersLongestMatch();
    void unknownExtensionFallsBackToFile();
    void unknownStemFallsBackToFile();

private:
    FileIconTheme m_theme;
};

void FileIconThemeTest::initTestCase()
{
    QVERIFY(m_theme.ready());
}

void FileIconThemeTest::mapsLanguageFilesToTypeIcons()
{
    QCOMPARE(m_theme.iconName(QStringLiteral("main.cpp"), false),
             QStringLiteral("type-cpp"));
    QCOMPARE(m_theme.iconName(QStringLiteral("Foo.H"), false),
             QStringLiteral("type-cpp"));
    QCOMPARE(m_theme.iconName(QStringLiteral("app.qml"), false),
             QStringLiteral("type-qml"));
    QCOMPARE(m_theme.iconName(QStringLiteral("lib.rs"), false),
             QStringLiteral("type-rust"));
    QCOMPARE(m_theme.iconName(QStringLiteral("main.py"), false),
             QStringLiteral("type-python"));
    QCOMPARE(m_theme.iconName(QStringLiteral("app.kt"), false),
             QStringLiteral("type-kotlin"));
}

void FileIconThemeTest::mapsExactFileNamesAheadOfExtensions()
{
    QCOMPARE(m_theme.iconName(QStringLiteral("package.json"), false),
             QStringLiteral("type-json"));
    QCOMPARE(m_theme.iconName(QStringLiteral("CMakeLists.txt"), false),
             QStringLiteral("type-build"));
    QCOMPARE(m_theme.iconName(QStringLiteral("README.md"), false),
             QStringLiteral("type-doc"));
    QCOMPARE(m_theme.iconName(QStringLiteral(".gitignore"), false),
             QStringLiteral("type-git"));
    QCOMPARE(m_theme.iconName(QStringLiteral("notes.txt"), false),
             QStringLiteral("file"));
}

void FileIconThemeTest::mapsDirectoriesToFolderGlyphs()
{
    QCOMPARE(m_theme.iconName(QStringLiteral("src"), true),
             QStringLiteral("folder"));
    QCOMPARE(m_theme.iconName(QStringLiteral("src"), true, true),
             QStringLiteral("folder-open"));
    QCOMPARE(m_theme.iconName(QStringLiteral("SpanTui"), true, false, true),
             QStringLiteral("folder"));
    QCOMPARE(m_theme.iconName(QStringLiteral("SpanTui"), true, true, true),
             QStringLiteral("folder-open"));
}

void FileIconThemeTest::emptyNameFallsBackToFile()
{
    QCOMPARE(m_theme.iconName(QString(), false), QStringLiteral("file"));
    QCOMPARE(m_theme.iconName(QStringLiteral("   "), false), QStringLiteral("file"));
}

void FileIconThemeTest::compoundExtensionPrefersLongestMatch()
{
    QCOMPARE(m_theme.iconName(QStringLiteral("foo.d.ts"), false),
             QStringLiteral("type-ts"));
}

void FileIconThemeTest::unknownExtensionFallsBackToFile()
{
    QCOMPARE(m_theme.iconName(QStringLiteral("unknown.xyz"), false),
             QStringLiteral("file"));
}

void FileIconThemeTest::unknownStemFallsBackToFile()
{
    FileIconTheme theme;
    QVERIFY(theme.ready());
    theme.setKnownNames({QStringLiteral("file"),
                         QStringLiteral("folder"),
                         QStringLiteral("folder-open")});
    QCOMPARE(theme.iconName(QStringLiteral("main.cpp"), false),
             QStringLiteral("file"));
    QCOMPARE(theme.iconName(QStringLiteral("src"), true, true),
             QStringLiteral("folder-open"));
}

QTEST_MAIN(FileIconThemeTest)
#include "file_icon_theme_test.moc"
