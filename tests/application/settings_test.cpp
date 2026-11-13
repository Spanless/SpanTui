#include "application/preferences.h"
#include "workspace/session_model.h"

#include <QClipboard>
#include <QDir>
#include <QFileInfo>
#include <QFontDatabase>
#include <QGuiApplication>
#include <QRawFont>
#include <QSignalSpy>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QTemporaryDir>
#include <QTest>

#include <algorithm>

using span::application::AppearanceSettings;
using span::application::Clipboard;
using span::application::Preferences;
using span::application::SettingsStore;
using span::application::TerminalSettings;
using span::application::WorkspaceSettings;

class SettingsTest final : public QObject {
    Q_OBJECT

private slots:
    void initTestCase();
    void loadsDefaultAppearanceSettings();
    void persistsTerminalTileClosePreference();
    void persistsAndValidatesScriptRunMode();
    void persistsToolPathOverrides();
    void persistsShellProfiles();
    void restoresSessionShellProfiles();
    void readOnlyPreferencesDoNotCreateDatabase();
    void readOnlyPreferencesDoNotMutateWorkspace();
    void restoresPerProjectActiveSessionAcrossRestart();
    void startsWithoutImplicitSession();
    void persistsAppearanceSettings();
    void normalizesColorSchemeSlots();
    void defaultsToInvalidWindowGeometry();
    void persistsWindowGeometry();
    void ignoresInvalidWindowGeometry();
    void rollsBackWindowGeometryWhenAnyWriteFails();
    void preservesSystemWheelScrollModes();
    void clampsFontSizes();
    void persistsAndSnapsEditorLineHeight();
    void terminalFontUsesFixedPitch();
    void nativeWeightKeepsSelectedPrimaryFamily();
    void preservesMissingTerminalFontAndReportsEffectiveFallback();
    void persistsUserColorSchemesAndSlots();
    void refusesInvalidColorSchemeSaves();
    void refusesRemovingColorSchemeInUse();
    void recoversFromCorruptStoredColorSchemes();
    void copiesTextToClipboard();

private:
    QString m_bundledTerminalFamily;
};

namespace {

bool openAt(Preferences &preferences, const QString &databasePath,
            SettingsStore::Mode mode = SettingsStore::Mode::Workspace, SessionModel *sessions = nullptr)
{
    preferences.store.setDatabasePathForTesting(databasePath);
    preferences.store.setSessionModel(sessions);
    return preferences.open(mode);
}

void seedSetting(const QString &databasePath, const QString &connectionName, const QString &key, const QString &value)
{
    {
        QSqlDatabase database = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), connectionName);
        database.setDatabaseName(databasePath);
        QVERIFY(database.open());
        QSqlQuery query(database);
        query.prepare(QStringLiteral("insert or replace into settings(key, value) values(?, ?)"));
        query.addBindValue(key);
        query.addBindValue(value);
        QVERIFY(query.exec());
        database.close();
    }
    QSqlDatabase::removeDatabase(connectionName);
}

bool containsFontWeight(const QVariantList &options, int weight)
{
    return std::any_of(options.cbegin(), options.cend(), [weight](const QVariant &option) {
        return option.toMap().value(QStringLiteral("weight")).toInt() == weight;
    });
}

} // namespace

void SettingsTest::initTestCase()
{
    const QString regularPath = QStringLiteral(":/fonts/SpanMono-Regular.ttf");
    const int regularId = QFontDatabase::addApplicationFont(regularPath);
    QVERIFY(regularId >= 0);
    const QStringList regularFamilies = QFontDatabase::applicationFontFamilies(regularId);
    QVERIFY(!regularFamilies.isEmpty());
    m_bundledTerminalFamily = regularFamilies.constFirst();
    QVERIFY(QFontDatabase::addApplicationFont(QStringLiteral(":/fonts/SpanMono-Bold.ttf")) >= 0);
    QVERIFY(QFontDatabase::addApplicationFont(QStringLiteral(":/fonts/SpanSans-Regular.ttf")) >= 0);
}

void SettingsTest::loadsDefaultAppearanceSettings()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    Preferences preferences;
    QVERIFY(openAt(preferences, dir.filePath(QStringLiteral("state.db"))));
    const AppearanceSettings &appearance = preferences.appearance;
    TerminalSettings &terminal = preferences.terminal;

    QCOMPARE(appearance.language(), QStringLiteral("zh_CN"));
    QCOMPARE(appearance.uiPrimaryFontFamily(), QStringLiteral("Span Sans"));
    QCOMPARE(appearance.uiFallbackFontFamily(), QStringLiteral("Microsoft YaHei UI"));
    QCOMPARE(appearance.uiFontSize(), 14);
    QVERIFY(!appearance.availableUiFontWeights().isEmpty());
    QVERIFY(containsFontWeight(appearance.availableUiFontWeights(), appearance.uiFontWeight()));
    QCOMPARE(appearance.uiAuxPrimaryFontFamily(), QStringLiteral("Span Sans"));
    QCOMPARE(appearance.uiAuxFallbackFontFamily(), QStringLiteral("Microsoft YaHei UI"));
    QCOMPARE(appearance.uiAuxFontSize(), 12);
    QVERIFY(!appearance.availableUiAuxFontWeights().isEmpty());
    QVERIFY(containsFontWeight(appearance.availableUiAuxFontWeights(), appearance.uiAuxFontWeight()));
    QCOMPARE(appearance.editorPrimaryFontFamily(), QStringLiteral("Span Mono"));
    QCOMPARE(appearance.editorFallbackFontFamily(), QStringLiteral("Microsoft YaHei Mono"));
    QCOMPARE(appearance.editorFontSize(), 13);
    QVERIFY(!appearance.availableEditorFontWeights().isEmpty());
    QVERIFY(!appearance.availableTerminalFontWeights().isEmpty());
    QVERIFY(containsFontWeight(appearance.availableEditorFontWeights(), appearance.editorFontWeight()));
    QVERIFY(containsFontWeight(appearance.availableTerminalFontWeights(), appearance.terminalFontWeight()));
    QVERIFY(!appearance.availableFonts().isEmpty());
    QVERIFY(!appearance.availableFixedPitchFonts().isEmpty());
    QCOMPARE(appearance.terminalPrimaryFontFamily(), m_bundledTerminalFamily);
    QVERIFY(appearance.availableFonts().contains(appearance.terminalPrimaryFontFamily()));
    QVERIFY(appearance.availableFonts().contains(appearance.terminalFallbackFontFamily()));
    QCOMPARE(appearance.terminalFontSize(), 13);
    QCOMPARE(appearance.editorLineHeight(), 1.0);
    QCOMPARE(appearance.themeMode(), QStringLiteral("system"));
    QVERIFY(!appearance.effectiveDark());
    QCOMPARE(terminal.colorSchemeDark(), QStringLiteral("Span Dark"));
    QCOMPARE(terminal.colorSchemeLight(), QStringLiteral("Span Light"));
    QCOMPARE(terminal.cursorStyle(), QStringLiteral("block"));
    QCOMPARE(terminal.refreshRate(), 60);
    QVERIFY(terminal.terminateOnTileClose());
    // Free numeric input: the stored value is what the user typed,
    // clamped to the supported range — presets are suggestions only.
    terminal.setRefreshRate(44);
    QCOMPARE(terminal.refreshRate(), 44);
    terminal.setRefreshRate(360);
    QCOMPARE(terminal.refreshRate(), 360);
    terminal.setRefreshRate(5);
    QCOMPARE(terminal.refreshRate(), 30);
    terminal.setRefreshRate(999);
    QCOMPARE(terminal.refreshRate(), 360);
    QCOMPARE(preferences.workspace.scriptRunMode(), QStringLiteral("projectSession"));
}

void SettingsTest::persistsTerminalTileClosePreference()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString databasePath = dir.filePath(QStringLiteral("state.db"));

    {
        Preferences preferences;
        QVERIFY(openAt(preferences, databasePath));
        QVERIFY(preferences.terminal.terminateOnTileClose());
        preferences.terminal.setTerminateOnTileClose(false);
        QCOMPARE(preferences.store.persistenceQueueDepth(), 1);
        preferences.store.saveNow();
        QCOMPARE(preferences.store.persistenceQueueDepth(), 0);
    }

    Preferences reloaded;
    QVERIFY(openAt(reloaded, databasePath));
    QVERIFY(!reloaded.terminal.terminateOnTileClose());
}

void SettingsTest::persistsAndValidatesScriptRunMode()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString databasePath = dir.filePath(QStringLiteral("state.db"));

    {
        Preferences preferences;
        QVERIFY(openAt(preferences, databasePath));
        QCOMPARE(preferences.workspace.scriptRunMode(), QStringLiteral("projectSession"));
        preferences.workspace.setScriptRunMode(QStringLiteral("independentWindow"));
        QCOMPARE(preferences.workspace.scriptRunMode(), QStringLiteral("independentWindow"));
        preferences.store.saveNow();
    }

    {
        Preferences reloaded;
        QVERIFY(openAt(reloaded, databasePath));
        QCOMPARE(reloaded.workspace.scriptRunMode(), QStringLiteral("independentWindow"));
        reloaded.workspace.setScriptRunMode(QStringLiteral("unsupported"));
        QCOMPARE(reloaded.workspace.scriptRunMode(), QStringLiteral("projectSession"));
    }

    seedSetting(databasePath, QStringLiteral("invalid_script_run_mode_test"), QStringLiteral("workspace.scriptRunMode"),
                QStringLiteral("floating"));

    Preferences invalidReload;
    QVERIFY(openAt(invalidReload, databasePath));
    QCOMPARE(invalidReload.workspace.scriptRunMode(), QStringLiteral("projectSession"));
}

void SettingsTest::persistsToolPathOverrides()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString databasePath = dir.filePath(QStringLiteral("state.db"));

    {
        Preferences preferences;
        QVERIFY(openAt(preferences, databasePath));
        QVERIFY(preferences.workspace.toolPathOverrides().isEmpty());
        QSignalSpy changed(&preferences.workspace, &WorkspaceSettings::toolPathOverridesChanged);
        preferences.workspace.setToolPathOverrides({{QStringLiteral("git"), QStringLiteral("C:/Tools/git.exe")}});
        QCOMPARE(changed.count(), 1);
        preferences.store.saveNow();
    }
    {
        Preferences reloaded;
        QVERIFY(openAt(reloaded, databasePath));
        QCOMPARE(reloaded.workspace.toolPathOverrides().value(QStringLiteral("git")).toString(),
                 QStringLiteral("C:/Tools/git.exe"));
    }

    // Empty and non-string entries carry no choice and are dropped on load.
    seedSetting(databasePath, QStringLiteral("tool_paths_seed"), QStringLiteral("tools.paths"),
                QStringLiteral("{\"git\":\"\",\"ssh\":3,\"sftp\":\"C:/OpenSSH/sftp.exe\"}"));
    Preferences seeded;
    QVERIFY(openAt(seeded, databasePath));
    QCOMPARE(seeded.workspace.toolPathOverrides(),
             (QVariantMap{{QStringLiteral("sftp"), QStringLiteral("C:/OpenSSH/sftp.exe")}}));
}

void SettingsTest::persistsShellProfiles()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString databasePath = dir.filePath(QStringLiteral("state.db"));
    const QVariantMap profiles{
        {QStringLiteral("default"), QStringLiteral("cmd")},
        {QStringLiteral("arguments"), QVariantMap{{QStringLiteral("pwsh"), QStringLiteral("-NoLogo -NoExit")}}},
    };
    {
        Preferences preferences;
        QVERIFY(openAt(preferences, databasePath));
        QVERIFY(preferences.terminal.shellProfiles().isEmpty());
        QSignalSpy changed(&preferences.terminal, &TerminalSettings::shellProfilesChanged);
        preferences.terminal.setShellProfiles(profiles);
        QCOMPARE(changed.count(), 1);
        preferences.store.saveNow();
    }
    {
        Preferences reloaded;
        QVERIFY(openAt(reloaded, databasePath));
        QCOMPARE(reloaded.terminal.shellProfiles(), profiles);
    }

    // A value that is not a JSON object carries no choice.
    seedSetting(databasePath, QStringLiteral("shell_profiles_seed"), QStringLiteral("terminal.shellProfiles"),
                QStringLiteral("[1,2]"));
    Preferences seeded;
    QVERIFY(openAt(seeded, databasePath));
    QVERIFY(seeded.terminal.shellProfiles().isEmpty());
}

void SettingsTest::restoresSessionShellProfiles()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString databasePath = dir.filePath(QStringLiteral("state.db"));
    QString followsDefault;
    QString pinned;
    {
        SessionModel sessions;
        Preferences preferences;
        QVERIFY(openAt(preferences, databasePath, SettingsStore::Mode::Workspace, &sessions));
        followsDefault = sessions.addSession(QStringLiteral("a"), dir.path(), dir.path());
        pinned = sessions.addSession(QStringLiteral("b"), dir.path(), dir.path(), QStringLiteral("git-bash"));
        QVERIFY(preferences.store.saveSessionsNow());
    }
    SessionModel sessions;
    Preferences preferences;
    QVERIFY(openAt(preferences, databasePath, SettingsStore::Mode::Workspace, &sessions));
    QCOMPARE(sessions.shellProfileForSession(followsDefault), QString());
    QCOMPARE(sessions.shellProfileForSession(pinned), QStringLiteral("git-bash"));
}

void SettingsTest::readOnlyPreferencesDoNotCreateDatabase()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString databasePath = dir.filePath(QStringLiteral("missing-parent/state.db"));

    {
        Preferences preferences;
        QVERIFY(openAt(preferences, databasePath, SettingsStore::Mode::ReadOnly));
        QCOMPARE(preferences.appearance.themeMode(), QStringLiteral("system"));
        preferences.appearance.setThemeMode(QStringLiteral("dark"));
        preferences.store.saveNow();
    }

    QVERIFY(!QFileInfo::exists(databasePath));
    QVERIFY(!QFileInfo::exists(QFileInfo(databasePath).absolutePath()));
}

void SettingsTest::readOnlyPreferencesDoNotMutateWorkspace()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString databasePath = dir.filePath(QStringLiteral("state.db"));

    {
        SessionModel sessions;
        Preferences workspace;
        QVERIFY(openAt(workspace, databasePath, SettingsStore::Mode::Workspace, &sessions));
        workspace.appearance.setThemeMode(QStringLiteral("dark"));
        sessions.addSession(QStringLiteral("shell"), dir.path(), dir.path());
        workspace.store.saveNow();
    }

    {
        Preferences terminalPreferences;
        QVERIFY(openAt(terminalPreferences, databasePath, SettingsStore::Mode::ReadOnly));
        QCOMPARE(terminalPreferences.appearance.themeMode(), QStringLiteral("dark"));
        terminalPreferences.appearance.setThemeMode(QStringLiteral("light"));
        terminalPreferences.store.saveNow();
    }

    SessionModel sessions;
    Preferences reloaded;
    QVERIFY(openAt(reloaded, databasePath, SettingsStore::Mode::Workspace, &sessions));
    QCOMPARE(reloaded.appearance.themeMode(), QStringLiteral("dark"));
    QCOMPARE(sessions.rowCount(), 1);
}

void SettingsTest::restoresPerProjectActiveSessionAcrossRestart()
{
    QTemporaryDir first;
    QTemporaryDir second;
    QVERIFY(first.isValid());
    QVERIFY(second.isValid());
    const QString databasePath = first.filePath(QStringLiteral("state.db"));
    QString firstProjectActive;
    QString secondProjectActive;
    {
        SessionModel sessions;
        Preferences preferences;
        QVERIFY(openAt(preferences, databasePath, SettingsStore::Mode::Workspace, &sessions));
        const QString a1 = sessions.addSession(QStringLiteral("a1"), first.path(), first.path());
        sessions.addSession(QStringLiteral("a2"), first.path(), first.path());
        sessions.addSession(QStringLiteral("b1"), second.path(), second.path());
        const QString b2 = sessions.addSession(QStringLiteral("b2"), second.path(), second.path());
        // Focus both projects, then return to a1. Each project remembers the
        // session that was focused when that project was last active.
        sessions.activateById(a1);
        sessions.activateById(b2);
        sessions.activateById(a1);
        firstProjectActive = a1;
        secondProjectActive = b2;
        QVERIFY(preferences.store.saveSessionsNow());
    }
    {
        SessionModel sessions;
        Preferences preferences;
        QVERIFY(openAt(preferences, databasePath, SettingsStore::Mode::Workspace, &sessions));
        QCOMPARE(sessions.rowCount(), 4);
        QCOMPARE(sessions.activeSessionId(), firstProjectActive);
        QCOMPARE(sessions.activeIndexForProject(first.path()), sessions.indexForSessionId(firstProjectActive));
        QCOMPARE(sessions.activeIndexForProject(second.path()), sessions.indexForSessionId(secondProjectActive));
    }
}

void SettingsTest::startsWithoutImplicitSession()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    SessionModel sessions;
    Preferences preferences;
    QVERIFY(openAt(preferences, dir.filePath(QStringLiteral("state.db")), SettingsStore::Mode::Workspace, &sessions));
    QCOMPARE(sessions.rowCount(), 0);
}

void SettingsTest::persistsAppearanceSettings()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString dbPath = dir.filePath(QStringLiteral("state.db"));
    QString primaryFont;
    QString fallbackFont;
    QString uiPrimaryFont;
    QString uiFallbackFont;
    QString uiAuxPrimaryFont;
    QString uiAuxFallbackFont;
    QString editorPrimaryFont;
    QString editorFallbackFont;
    int uiFontWeight = QFont::Normal;
    int uiAuxFontWeight = QFont::Normal;
    int editorFontWeight = QFont::Normal;
    int terminalFontWeight = QFont::Normal;

    {
        Preferences preferences;
        QVERIFY(openAt(preferences, dbPath));
        AppearanceSettings &state = preferences.appearance;
        TerminalSettings &terminal = preferences.terminal;

        // Not the default, so the reload proves it was stored.
        state.setLanguage(QStringLiteral("en"));
        uiPrimaryFont = state.availableFonts().contains(QStringLiteral("Segoe UI")) ? QStringLiteral("Segoe UI")
                                                                                    : state.availableFonts().first();
        uiFallbackFont = state.availableFonts().contains(QStringLiteral("Microsoft YaHei UI"))
            ? QStringLiteral("Microsoft YaHei UI")
            : state.availableFonts().last();
        state.setUiPrimaryFontFamily(uiPrimaryFont);
        state.setUiFallbackFontFamily(uiFallbackFont);
        uiPrimaryFont = state.uiPrimaryFontFamily();
        uiFallbackFont = state.uiFallbackFontFamily();
        state.setUiFontSize(15);
        uiFontWeight = state.availableUiFontWeights().last().toMap().value(QStringLiteral("weight")).toInt();
        state.setUiFontWeight(uiFontWeight);
        uiAuxPrimaryFont = state.availableFonts().contains(QStringLiteral("Arial")) ? QStringLiteral("Arial")
                                                                                    : state.availableFonts().last();
        uiAuxFallbackFont = uiFallbackFont;
        state.setUiAuxPrimaryFontFamily(uiAuxPrimaryFont);
        state.setUiAuxFallbackFontFamily(uiAuxFallbackFont);
        uiAuxPrimaryFont = state.uiAuxPrimaryFontFamily();
        uiAuxFallbackFont = state.uiAuxFallbackFontFamily();
        state.setUiAuxFontSize(11);
        uiAuxFontWeight = state.availableUiAuxFontWeights().last().toMap().value(QStringLiteral("weight")).toInt();
        state.setUiAuxFontWeight(uiAuxFontWeight);
        editorPrimaryFont = state.availableFixedPitchFonts().contains(QStringLiteral("Cascadia Code"))
            ? QStringLiteral("Cascadia Code")
            : state.availableFixedPitchFonts().first();
        editorFallbackFont = state.availableFonts().contains(QStringLiteral("Microsoft YaHei Mono"))
            ? QStringLiteral("Microsoft YaHei Mono")
            : state.availableFonts().first();
        state.setEditorPrimaryFontFamily(editorPrimaryFont);
        state.setEditorFallbackFontFamily(editorFallbackFont);
        editorPrimaryFont = state.editorPrimaryFontFamily();
        editorFallbackFont = state.editorFallbackFontFamily();
        state.setEditorFontSize(17);
        editorFontWeight = state.availableEditorFontWeights().last().toMap().value(QStringLiteral("weight")).toInt();
        state.setEditorFontWeight(editorFontWeight);
        primaryFont = state.availableFonts().first();
        fallbackFont = state.availableFonts().first();
        state.setTerminalPrimaryFontFamily(primaryFont);
        state.setTerminalFallbackFontFamily(fallbackFont);
        state.setTerminalFontSize(16);
        terminalFontWeight =
            state.availableTerminalFontWeights().last().toMap().value(QStringLiteral("weight")).toInt();
        state.setTerminalFontWeight(terminalFontWeight);
        state.setEditorLineHeight(1.5);
        QVERIFY(
            terminal.saveColorScheme(QStringLiteral("My Scheme"), terminal.colorScheme(QStringLiteral("Campbell"))));
        terminal.setColorSchemeDark(QStringLiteral("My Scheme"));
        terminal.setCursorStyle(QStringLiteral("underline"));
        terminal.setRefreshRate(360);
        state.setThemeMode(QStringLiteral("dark"));
        terminal.setColorSchemeLight(QStringLiteral("Ocean Light"));
        preferences.store.saveNow();
    }

    Preferences preferences;
    QVERIFY(openAt(preferences, dbPath));
    const AppearanceSettings &reloaded = preferences.appearance;
    const TerminalSettings &terminal = preferences.terminal;

    QCOMPARE(reloaded.language(), QStringLiteral("en"));
    QCOMPARE(reloaded.uiPrimaryFontFamily(), uiPrimaryFont);
    QCOMPARE(reloaded.uiFallbackFontFamily(), uiFallbackFont);
    QCOMPARE(reloaded.uiFontSize(), 15);
    QCOMPARE(reloaded.uiFontWeight(), uiFontWeight);
    QCOMPARE(int(reloaded.uiFont(reloaded.uiFontSize()).weight()), uiFontWeight);
    QCOMPARE(reloaded.uiAuxPrimaryFontFamily(), uiAuxPrimaryFont);
    QCOMPARE(reloaded.uiAuxFallbackFontFamily(), uiAuxFallbackFont);
    QCOMPARE(reloaded.uiAuxFontSize(), 11);
    QCOMPARE(reloaded.uiAuxFontWeight(), uiAuxFontWeight);
    QCOMPARE(int(reloaded.uiAuxFont(reloaded.uiAuxFontSize()).weight()), uiAuxFontWeight);
    QCOMPARE(reloaded.editorPrimaryFontFamily(), editorPrimaryFont);
    QCOMPARE(reloaded.editorFallbackFontFamily(), editorFallbackFont);
    QCOMPARE(reloaded.editorFontSize(), 17);
    QCOMPARE(reloaded.editorFontWeight(), editorFontWeight);
    QCOMPARE(int(reloaded.editorFont(reloaded.editorFontSize()).weight()), editorFontWeight);
    QCOMPARE(reloaded.terminalPrimaryFontFamily(), primaryFont);
    QCOMPARE(reloaded.terminalFallbackFontFamily(), fallbackFont);
    QCOMPARE(reloaded.terminalFontSize(), 16);
    QCOMPARE(reloaded.terminalFontWeight(), terminalFontWeight);
    QCOMPARE(int(reloaded.terminalFont().weight()), terminalFontWeight);
    QCOMPARE(reloaded.editorLineHeight(), 1.5);
    QCOMPARE(terminal.colorSchemeDark(), QStringLiteral("My Scheme"));
    QCOMPARE(terminal.colorSchemeLight(), QStringLiteral("Ocean Light"));
    QCOMPARE(terminal.colorScheme(QStringLiteral("My Scheme")).value(QStringLiteral("cursor")).value<QColor>(),
             QColor(QStringLiteral("#ffffff")));
    QCOMPARE(terminal.cursorStyle(), QStringLiteral("underline"));
    QCOMPARE(terminal.refreshRate(), 360);
    QCOMPARE(reloaded.themeMode(), QStringLiteral("dark"));
    QVERIFY(reloaded.effectiveDark());
}

void SettingsTest::normalizesColorSchemeSlots()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    Preferences preferences;
    QVERIFY(openAt(preferences, dir.filePath(QStringLiteral("state.db"))));
    TerminalSettings &terminal = preferences.terminal;

    terminal.setColorSchemeDark(QStringLiteral("Ember Dark"));
    QCOMPARE(terminal.colorSchemeDark(), QStringLiteral("Ember Dark"));
    terminal.setColorSchemeDark(QStringLiteral("not-a-scheme"));
    QCOMPARE(terminal.colorSchemeDark(), QStringLiteral("Ember Dark"));
    terminal.setColorSchemeLight(QStringLiteral("Forest Light"));
    QCOMPARE(terminal.colorSchemeLight(), QStringLiteral("Forest Light"));
    terminal.setColorSchemeLight(QStringLiteral("not-a-scheme"));
    QCOMPARE(terminal.colorSchemeLight(), QStringLiteral("Forest Light"));
}

void SettingsTest::defaultsToInvalidWindowGeometry()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    Preferences preferences;
    QVERIFY(openAt(preferences, dir.filePath(QStringLiteral("state.db"))));

    QVERIFY(!preferences.store.windowGeometry().isValid());
    QVERIFY(!preferences.store.windowMaximized());
}

void SettingsTest::persistsWindowGeometry()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString dbPath = dir.filePath(QStringLiteral("state.db"));

    {
        Preferences preferences;
        QVERIFY(openAt(preferences, dbPath));
        preferences.store.saveWindowGeometry(120, 80, 1320, 820, true);
    }

    Preferences reloaded;
    QVERIFY(openAt(reloaded, dbPath));
    QCOMPARE(reloaded.store.windowGeometry(), QRect(120, 80, 1320, 820));
    QVERIFY(reloaded.store.windowMaximized());
}

void SettingsTest::rollsBackWindowGeometryWhenAnyWriteFails()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString dbPath = dir.filePath(QStringLiteral("state.db"));

    {
        Preferences preferences;
        QVERIFY(openAt(preferences, dbPath));
        preferences.store.saveWindowGeometry(120, 80, 1320, 820, false);

        const QString blocker = QStringLiteral("geometry_blocker");
        {
            QSqlDatabase database = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), blocker);
            database.setDatabaseName(dbPath);
            QVERIFY(database.open());
            QSqlQuery query(database);
            QVERIFY(query.exec(QStringLiteral("create trigger block_maximized before insert on settings "
                                              "when new.key = 'window.maximized' "
                                              "begin select raise(abort, 'blocked'); end")));
            QVERIFY(query.exec(QStringLiteral("create trigger block_maximized_update before update on settings "
                                              "when new.key = 'window.maximized' "
                                              "begin select raise(abort, 'blocked'); end")));
            database.close();
        }
        QSqlDatabase::removeDatabase(blocker);

        QSignalSpy errors(&preferences.store, &SettingsStore::errorOccurred);
        preferences.store.saveWindowGeometry(10, 20, 640, 480, true);
        QVERIFY(!errors.isEmpty());
        // The failed batch stays staged for the next attempt.
        QCOMPARE(preferences.store.persistenceQueueDepth(), 1);
    }

    Preferences reloaded;
    QVERIFY(openAt(reloaded, dbPath));
    // One failing key rolls back the whole batch.
    QCOMPARE(reloaded.store.windowGeometry(), QRect(120, 80, 1320, 820));
    QVERIFY(!reloaded.store.windowMaximized());
}

void SettingsTest::ignoresInvalidWindowGeometry()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString dbPath = dir.filePath(QStringLiteral("state.db"));

    {
        Preferences preferences;
        QVERIFY(openAt(preferences, dbPath));
        preferences.store.saveWindowGeometry(120, 80, 1320, 820, false);
        // A zero-sized rect (e.g. read from a window that was never shown)
        // must not clobber the previously saved geometry.
        preferences.store.saveWindowGeometry(0, 0, 0, 0, true);
    }

    Preferences reloaded;
    QVERIFY(openAt(reloaded, dbPath));
    QCOMPARE(reloaded.store.windowGeometry(), QRect(120, 80, 1320, 820));
    QVERIFY(reloaded.store.windowMaximized());
}

void SettingsTest::preservesSystemWheelScrollModes()
{
    AppearanceSettings appearance;

    appearance.setSystemWheelScrollLines(0);
    QCOMPARE(appearance.wheelScrollLines(), 0);

    appearance.setSystemWheelScrollLines(-1);
    QCOMPARE(appearance.wheelScrollLines(), -1);

    appearance.setSystemWheelScrollLines(20000);
    QCOMPARE(appearance.wheelScrollLines(), 20000);
}

void SettingsTest::clampsFontSizes()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    Preferences preferences;
    QVERIFY(openAt(preferences, dir.filePath(QStringLiteral("state.db"))));
    AppearanceSettings &state = preferences.appearance;

    state.setUiFontSize(2);
    state.setEditorFontSize(100);
    state.setTerminalFontSize(100);

    QCOMPARE(state.uiFontSize(), 10);
    QCOMPARE(state.editorFontSize(), 22);
    QCOMPARE(state.terminalFontSize(), 22);

    state.setUiFontWeight(1);
    state.setEditorFontWeight(1);
    state.setTerminalFontWeight(1000);
    QCOMPARE(state.uiFontWeight(),
             state.availableUiFontWeights().first().toMap().value(QStringLiteral("weight")).toInt());
    QCOMPARE(state.editorFontWeight(),
             state.availableEditorFontWeights().first().toMap().value(QStringLiteral("weight")).toInt());
    QCOMPARE(state.terminalFontWeight(),
             state.availableTerminalFontWeights().last().toMap().value(QStringLiteral("weight")).toInt());
}

void SettingsTest::persistsAndSnapsEditorLineHeight()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString dbPath = dir.filePath(QStringLiteral("state.db"));

    {
        Preferences preferences;
        QVERIFY(openAt(preferences, dbPath));
        AppearanceSettings &state = preferences.appearance;

        state.setEditorLineHeight(1.2);
        QCOMPARE(state.editorLineHeight(), 1.25);
        state.setEditorLineHeight(1.9);
        QCOMPARE(state.editorLineHeight(), 2.0);
        state.setEditorLineHeight(1.5);
        preferences.store.saveNow();
    }

    Preferences reloaded;
    QVERIFY(openAt(reloaded, dbPath));
    QCOMPARE(reloaded.appearance.editorLineHeight(), 1.5);
}

void SettingsTest::terminalFontUsesFixedPitch()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    Preferences preferences;
    QVERIFY(openAt(preferences, dir.filePath(QStringLiteral("state.db"))));
    AppearanceSettings &state = preferences.appearance;

    const QFont terminalFont = state.terminalFont();
    QCOMPARE(terminalFont.styleHint(), QFont::Monospace);
    QVERIFY(terminalFont.fixedPitch());
    QVERIFY(!terminalFont.kerning());

    // Editor primary+fallback become a resolved family list (with platform tails).
    // Consolas and Microsoft YaHei ship with every supported Windows.
    state.setEditorPrimaryFontFamily(QStringLiteral("Consolas"));
    state.setEditorFallbackFontFamily(QStringLiteral("Microsoft YaHei"));
    const QFont editor = state.editorFont(state.editorFontSize());
    QCOMPARE(editor.styleHint(), QFont::Monospace);
    QVERIFY(editor.fixedPitch());
    QVERIFY(!editor.families().isEmpty());
    QCOMPARE(state.editorFont(11).pixelSize(), 11);
    QCOMPARE(state.uiFont(14).pixelSize(), 14);

    // Resolved UI list drops missing faces and still paints with installed ones.
    state.setUiPrimaryFontFamily(QStringLiteral("SpanTui Missing UI Face"));
    state.setUiFallbackFontFamily(QStringLiteral("Another Missing Face"));
    const QStringList uiList = state.uiFont(state.uiFontSize()).families();
    QVERIFY(!uiList.isEmpty());
    QVERIFY(!uiList.contains(QStringLiteral("SpanTui Missing UI Face")));
    QVERIFY(!state.uiPrimaryFontFamily().isEmpty());
    QVERIFY(!state.uiFallbackFontFamily().isEmpty());
    QVERIFY(state.isFixedPitch(QStringLiteral("")));
    QVERIFY(state.isFixedPitch(QStringLiteral("Consolas")));
    QVERIFY(!state.isFixedPitch(QStringLiteral("Arial")));

    // A proportional terminal face is the user's call and is kept.
    QVERIFY(state.availableFonts().contains(QStringLiteral("Arial")));
    state.setTerminalPrimaryFontFamily(QStringLiteral("Arial"));
    QCOMPARE(state.terminalPrimaryFontFamily(), QStringLiteral("Arial"));
    QCOMPARE(state.terminalFont().families().constFirst(), QStringLiteral("Arial"));
}

void SettingsTest::nativeWeightKeepsSelectedPrimaryFamily()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    Preferences preferences;
    QVERIFY(openAt(preferences, dir.filePath(QStringLiteral("state.db"))));
    AppearanceSettings &state = preferences.appearance;

    const auto verifyPrimary = [](const QFont &font, const QString &primary) {
        QCOMPARE(font.families().constFirst(), primary);
        QCOMPARE(QRawFont::fromFont(font).familyName(), primary);
        QVERIFY(!font.styleName().isEmpty());
    };
    const auto selectedFace = [](const QVariantList &options) { return options.constLast().toMap(); };

    // The bundled mono family ships a Regular and a Bold face: picking the
    // heavier face must keep the family instead of falling back.
    const QString family = m_bundledTerminalFamily;
    QVERIFY(state.availableFixedPitchFonts().contains(family));
    {
        state.setUiPrimaryFontFamily(family);
        const QVariantMap face = selectedFace(state.availableUiFontWeights());
        QVERIFY(face.value(QStringLiteral("weight")).toInt() > QFont::Normal);
        state.setUiFontWeight(face.value(QStringLiteral("weight")).toInt());
        const QFont font = state.uiFont(state.uiFontSize());
        verifyPrimary(font, family);
        QCOMPARE(int(font.weight()), face.value(QStringLiteral("weight")).toInt());
        QCOMPARE(font.styleName(), face.value(QStringLiteral("style")).toString());
    }
    {
        state.setEditorPrimaryFontFamily(family);
        const QVariantMap face = selectedFace(state.availableEditorFontWeights());
        state.setEditorFontWeight(face.value(QStringLiteral("weight")).toInt());
        const QFont font = state.editorFont(state.editorFontSize());
        verifyPrimary(font, family);
        QCOMPARE(int(font.weight()), face.value(QStringLiteral("weight")).toInt());
        QCOMPARE(font.styleName(), face.value(QStringLiteral("style")).toString());
    }
    {
        state.setTerminalPrimaryFontFamily(family);
        const QVariantMap face = selectedFace(state.availableTerminalFontWeights());
        state.setTerminalFontWeight(face.value(QStringLiteral("weight")).toInt());
        const QFont font = state.terminalFont();
        verifyPrimary(font, family);
        QCOMPARE(int(font.weight()), face.value(QStringLiteral("weight")).toInt());
        QCOMPARE(font.styleName(), face.value(QStringLiteral("style")).toString());
    }
}

void SettingsTest::preservesMissingTerminalFontAndReportsEffectiveFallback()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString dbPath = dir.filePath(QStringLiteral("state.db"));
    {
        Preferences preferences;
        QVERIFY(openAt(preferences, dbPath));
    }

    seedSetting(dbPath, QStringLiteral("missing_terminal_font_seed"), QStringLiteral("terminalPrimaryFontFamily"),
                QStringLiteral("SpanTui Missing Configured Mono"));
    seedSetting(dbPath, QStringLiteral("terminal_fallback_seed"), QStringLiteral("terminalFallbackFontFamily"),
                m_bundledTerminalFamily);

    Preferences preferences;
    QVERIFY(openAt(preferences, dbPath));
    const AppearanceSettings &reloaded = preferences.appearance;
    QCOMPARE(reloaded.terminalPrimaryFontFamily(), QStringLiteral("SpanTui Missing Configured Mono"));
    QCOMPARE(reloaded.terminalFallbackFontFamily(), m_bundledTerminalFamily);
    QCOMPARE(reloaded.terminalFont().families().constFirst(), m_bundledTerminalFamily);

    preferences.store.saveNow();
    QCOMPARE(reloaded.terminalPrimaryFontFamily(), QStringLiteral("SpanTui Missing Configured Mono"));
}

void SettingsTest::persistsUserColorSchemesAndSlots()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString databasePath = dir.filePath(QStringLiteral("state.db"));
    {
        Preferences preferences;
        QVERIFY(openAt(preferences, databasePath));
        TerminalSettings &terminal = preferences.terminal;

        QVariantMap campbell = terminal.colorScheme(QStringLiteral("Campbell"));
        // The editor's state layer edits ride the same save path.
        campbell.insert(QStringLiteral("uiSelection"), QColor(QStringLiteral("#30b45a1c")));
        QSignalSpy schemesSpy(&terminal, &TerminalSettings::colorSchemesChanged);
        QVERIFY(terminal.saveColorScheme(QStringLiteral("Workbench"), campbell));
        QCOMPARE(schemesSpy.count(), 1);
        terminal.setColorSchemeLight(QStringLiteral("Workbench"));
        preferences.store.saveNow();
    }

    Preferences preferences;
    QVERIFY(openAt(preferences, databasePath));
    const TerminalSettings &reloaded = preferences.terminal;
    QCOMPARE(reloaded.colorSchemeDark(), QStringLiteral("Span Dark"));
    QCOMPARE(reloaded.colorSchemeLight(), QStringLiteral("Workbench"));

    const QVariantMap stored = reloaded.colorScheme(QStringLiteral("Workbench"));
    QCOMPARE(stored.value(QStringLiteral("builtIn")).toBool(), false);
    QCOMPARE(stored.value(QStringLiteral("background")).value<QColor>(), QColor(QStringLiteral("#0c0c0c")));
    QCOMPARE(stored.value(QStringLiteral("uiBackground")).value<QColor>(), QColor(QStringLiteral("#0c0c0c")));
    QCOMPARE(stored.value(QStringLiteral("ansi")).toList().size(), 16);
    QCOMPARE(stored.value(QStringLiteral("uiSelection")).value<QColor>(), QColor(QStringLiteral("#30b45a1c")));
    QVERIFY(stored.value(QStringLiteral("uiHover")).value<QColor>().isValid());

    // The light slot resolves through the user scheme, not the defaults.
    preferences.appearance.setThemeMode(QStringLiteral("light"));
    QVERIFY(!preferences.appearance.effectiveDark());
    QCOMPARE(reloaded.surfaceColors().value(QStringLiteral("background")).value<QColor>(),
             QColor(QStringLiteral("#0c0c0c")));
}

void SettingsTest::refusesInvalidColorSchemeSaves()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    Preferences preferences;
    QVERIFY(openAt(preferences, dir.filePath(QStringLiteral("state.db"))));
    TerminalSettings &terminal = preferences.terminal;

    QSignalSpy errors(&terminal, &TerminalSettings::errorOccurred);
    const QVariantMap campbell = terminal.colorScheme(QStringLiteral("Campbell"));

    QVERIFY(!terminal.saveColorScheme(QStringLiteral("   "), campbell));
    QVERIFY(!terminal.saveColorScheme(QStringLiteral("Campbell"), campbell));

    QVariantMap broken = campbell;
    broken.insert(QStringLiteral("background"), QStringLiteral("not-a-color"));
    QVERIFY(!terminal.saveColorScheme(QStringLiteral("Broken"), broken));

    QVariantMap shortAnsi = campbell;
    shortAnsi.insert(QStringLiteral("ansi"), QVariantList{QColor(QStringLiteral("#000000"))});
    QVERIFY(!terminal.saveColorScheme(QStringLiteral("Short"), shortAnsi));

    QCOMPARE(errors.count(), 4);
    QCOMPARE(terminal.colorScheme(QStringLiteral("Broken")), QVariantMap());
}

void SettingsTest::refusesRemovingColorSchemeInUse()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    Preferences preferences;
    QVERIFY(openAt(preferences, dir.filePath(QStringLiteral("state.db"))));
    TerminalSettings &terminal = preferences.terminal;

    const QVariantMap campbell = terminal.colorScheme(QStringLiteral("Campbell"));
    QVERIFY(terminal.saveColorScheme(QStringLiteral("Workbench"), campbell));
    terminal.setColorSchemeDark(QStringLiteral("Workbench"));

    QSignalSpy errors(&terminal, &TerminalSettings::errorOccurred);
    QVERIFY(!terminal.removeColorScheme(QStringLiteral("Workbench")));
    QVERIFY(!terminal.removeColorScheme(QStringLiteral("Campbell")));
    QVERIFY(!terminal.removeColorScheme(QStringLiteral("Unknown")));
    QCOMPARE(errors.count(), 3);

    terminal.setColorSchemeDark(QStringLiteral("Span Dark"));
    QVERIFY(terminal.removeColorScheme(QStringLiteral("Workbench")));
    QCOMPARE(terminal.colorScheme(QStringLiteral("Workbench")), QVariantMap());
}

void SettingsTest::recoversFromCorruptStoredColorSchemes()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString databasePath = dir.filePath(QStringLiteral("state.db"));
    {
        Preferences preferences;
        QVERIFY(openAt(preferences, databasePath));
    }

    const QString validScheme = QStringLiteral("{\"foreground\":\"#ff000000\",\"background\":\"#ff0c0c0c\","
                                               "\"cursor\":\"#ffffffff\",\"selection\":\"#ffffffff\","
                                               "\"ansi\":[\"#0c0c0c\",\"#c50f1f\",\"#13a10e\",\"#c19c00\","
                                               "\"#0037da\",\"#881798\",\"#3a96dd\",\"#cccccc\","
                                               "\"#767676\",\"#e74856\",\"#16c60c\",\"#f9f1a5\","
                                               "\"#3b78ff\",\"#b4009e\",\"#61d6d6\",\"#f2f2f2\"]}");
    seedSetting(databasePath, QStringLiteral("corrupt_scheme_seed"), QStringLiteral("terminal.colorSchemes"),
                QStringLiteral("{\"Good\":%1,\"Bad\":{\"foreground\":\"nope\"}}").arg(validScheme));
    seedSetting(databasePath, QStringLiteral("missing_slot_seed"), QStringLiteral("terminal.colorSchemeDark"),
                QStringLiteral("Workbench"));

    Preferences preferences;
    QVERIFY(openAt(preferences, databasePath));
    const TerminalSettings &reloaded = preferences.terminal;

    // The unknown slot falls back; the valid stored scheme survives and the
    // broken entry is dropped without taking the collection down.
    QCOMPARE(reloaded.colorSchemeDark(), QStringLiteral("Span Dark"));
    QCOMPARE(reloaded.colorScheme(QStringLiteral("Good")).value(QStringLiteral("background")).value<QColor>(),
             QColor(QStringLiteral("#0c0c0c")));
    QCOMPARE(reloaded.colorScheme(QStringLiteral("Bad")), QVariantMap());
}

void SettingsTest::copiesTextToClipboard()
{
    const Clipboard clipboard;
    clipboard.copyText(QStringLiteral("span_test_copy"));
    QCOMPARE(QGuiApplication::clipboard()->text(), QStringLiteral("span_test_copy"));
}

QTEST_MAIN(SettingsTest)

#include "settings_test.moc"
