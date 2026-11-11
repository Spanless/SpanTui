#include "application/preferences.h"
#include "application/shortcut_registry.h"

#include <QSignalSpy>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QTemporaryDir>
#include <QTest>

using span::application::Preferences;
using span::application::ShortcutRegistry;

class ShortcutRegistryTest final : public QObject {
    Q_OBJECT

private slots:
    void exposesDefaults();
    void matchesKeyEvents();
    void persistsOverridesAcrossRestart();
    void sharesChordsBetweenActions();
    void rejectsUnacceptableSequences();
    void unbindsWithEmptySequence();
    void resetsSingleAndAllBindings();
    void ignoresCorruptStoredBindings();
    void recordsChordsFromKeys();
};

namespace {

bool openAt(Preferences &preferences, const QString &databasePath)
{
    preferences.store.setDatabasePathForTesting(databasePath);
    return preferences.open(span::application::SettingsStore::Mode::Workspace);
}

QVariantMap entry(const ShortcutRegistry &registry, const QString &id)
{
    const QVariantList catalog = registry.catalog();
    for (const QVariant &item : catalog) {
        if (item.toMap().value(QStringLiteral("id")).toString() == id) {
            return item.toMap();
        }
    }
    return {};
}

// setBinding(id, text) for each text: its error, or "ok" when accepted.
QStringList bindingErrors(ShortcutRegistry &registry, const QString &id, const QStringList &texts)
{
    QStringList errors;
    for (const QString &text : texts) {
        const QVariantMap result = registry.setBinding(id, text);
        errors.append(result.value(QStringLiteral("ok")).toBool() ? QStringLiteral("ok")
                                                                  : result.value(QStringLiteral("error")).toString());
    }
    return errors;
}

void seedSetting(const QString &databasePath, const QString &key, const QString &value)
{
    const QString connection = QStringLiteral("shortcut-seed");
    {
        QSqlDatabase database = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), connection);
        database.setDatabaseName(databasePath);
        QVERIFY(database.open());
        QSqlQuery query(database);
        query.prepare(QStringLiteral("insert or replace into settings(key, value) values(?, ?)"));
        query.addBindValue(key);
        query.addBindValue(value);
        QVERIFY(query.exec());
        database.close();
    }
    QSqlDatabase::removeDatabase(connection);
}

} // namespace

void ShortcutRegistryTest::exposesDefaults()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    Preferences state;
    QVERIFY(openAt(state, dir.filePath(QStringLiteral("state.db"))));
    ShortcutRegistry registry(&state.workspace);

    QCOMPARE(registry.sequence(QStringLiteral("terminal.find")), QStringLiteral("Ctrl+Shift+F"));
    QCOMPARE(registry.sequence(QStringLiteral("editor.find")), QStringLiteral("Ctrl+F"));
    QCOMPARE(registry.sequence(QStringLiteral("no.such.action")), QString());
    QCOMPARE(registry.catalog().size(), ShortcutRegistry::actions().size());
    const QVariantMap find = entry(registry, QStringLiteral("terminal.find"));
    QCOMPARE(find.value(QStringLiteral("scope")).toString(), QStringLiteral("terminal"));
    QCOMPARE(find.value(QStringLiteral("customized")).toBool(), false);

    const QVariantMap terminal = registry.terminalBindings();
    QCOMPARE(terminal.value(QStringLiteral("terminal.copy")).toString(), QStringLiteral("Ctrl+Shift+C"));
    QVERIFY(!terminal.contains(QStringLiteral("editor.find")));
}

void ShortcutRegistryTest::matchesKeyEvents()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    Preferences state;
    QVERIFY(openAt(state, dir.filePath(QStringLiteral("state.db"))));
    ShortcutRegistry registry(&state.workspace);

    const int ctrlShift = int(Qt::ControlModifier | Qt::ShiftModifier);
    QVERIFY(registry.matches(QStringLiteral("terminal.find"), Qt::Key_F, ctrlShift));
    QVERIFY(!registry.matches(QStringLiteral("terminal.find"), Qt::Key_F, int(Qt::ControlModifier)));
    // Keypad state is not part of a chord.
    QVERIFY(registry.matches(QStringLiteral("editor.find"), Qt::Key_F,
                             int(Qt::ControlModifier | Qt::KeypadModifier)));
    QVERIFY(!registry.matches(QStringLiteral("editor.find"), Qt::Key_Control, int(Qt::ControlModifier)));
}

void ShortcutRegistryTest::persistsOverridesAcrossRestart()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("state.db"));
    {
        Preferences state;
        QVERIFY(openAt(state, path));
        ShortcutRegistry registry(&state.workspace);
        QSignalSpy changed(&registry, &ShortcutRegistry::bindingsChanged);
        const QVariantMap result = registry.setBinding(QStringLiteral("terminal.find"),
                                                       QStringLiteral("Alt+F3"));
        QVERIFY(result.value(QStringLiteral("ok")).toBool());
        QCOMPARE(changed.count(), 1);
        QCOMPARE(registry.sequence(QStringLiteral("terminal.find")), QStringLiteral("Alt+F3"));
        QVERIFY(entry(registry, QStringLiteral("terminal.find")).value(QStringLiteral("customized")).toBool());
        state.store.saveNow();
    }
    Preferences reloaded;
    QVERIFY(openAt(reloaded, path));
    ShortcutRegistry registry(&reloaded.workspace);
    QCOMPARE(registry.sequence(QStringLiteral("terminal.find")), QStringLiteral("Alt+F3"));
    QCOMPARE(registry.terminalBindings().value(QStringLiteral("terminal.find")).toString(),
             QStringLiteral("Alt+F3"));
}

void ShortcutRegistryTest::sharesChordsBetweenActions()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    Preferences state;
    QVERIFY(openAt(state, dir.filePath(QStringLiteral("state.db"))));
    ShortcutRegistry registry(&state.workspace);
    const auto sharedWith = [&registry](const QString &id) {
        return entry(registry, id).value(QStringLiteral("sharedWith")).toStringList();
    };

    // One chord may carry several actions; the terminal runs each that applies.
    QVERIFY(registry.setBinding(QStringLiteral("terminal.find"), QStringLiteral("Ctrl+Shift+C"))
                .value(QStringLiteral("ok")).toBool());
    QCOMPARE(registry.sequence(QStringLiteral("terminal.find")), QStringLiteral("Ctrl+Shift+C"));
    QCOMPARE(registry.terminalBindings().value(QStringLiteral("terminal.find")).toString(),
             QStringLiteral("Ctrl+Shift+C"));
    QCOMPARE(sharedWith(QStringLiteral("terminal.find")), QStringList{QStringLiteral("terminal.copy")});
    QCOMPARE(sharedWith(QStringLiteral("terminal.copy")), QStringList{QStringLiteral("terminal.find")});

    // Editor and terminal never receive the same key event, so they share nothing.
    QVERIFY(registry.setBinding(QStringLiteral("terminal.paste"), QStringLiteral("Ctrl+G"))
                .value(QStringLiteral("ok")).toBool());
    QVERIFY(sharedWith(QStringLiteral("terminal.paste")).isEmpty());
    QVERIFY(sharedWith(QStringLiteral("editor.goToLine")).isEmpty());
}

void ShortcutRegistryTest::rejectsUnacceptableSequences()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    Preferences state;
    QVERIFY(openAt(state, dir.filePath(QStringLiteral("state.db"))));
    ShortcutRegistry registry(&state.workspace);

    QCOMPARE(bindingErrors(registry, QStringLiteral("editor.find"),
                           {QStringLiteral("F"), QStringLiteral("Shift+F"), QStringLiteral("Ctrl+K, Ctrl+F"),
                            QStringLiteral("Nonsense+Key")}),
             (QStringList{QStringLiteral("invalid"), QStringLiteral("invalid"), QStringLiteral("invalid"),
                          QStringLiteral("invalid")}));
    QCOMPARE(registry.setBinding(QStringLiteral("missing"), QStringLiteral("Ctrl+F"))
                 .value(QStringLiteral("error")).toString(),
             QStringLiteral("unknown"));
    QVERIFY(registry.setBinding(QStringLiteral("editor.find"), QStringLiteral("F7"))
                .value(QStringLiteral("ok")).toBool());
    QVERIFY(state.workspace.shortcutOverrides().contains(QStringLiteral("editor.find")));
}

void ShortcutRegistryTest::unbindsWithEmptySequence()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    Preferences state;
    QVERIFY(openAt(state, dir.filePath(QStringLiteral("state.db"))));
    ShortcutRegistry registry(&state.workspace);

    QVERIFY(registry.setBinding(QStringLiteral("terminal.paste"), QString())
                .value(QStringLiteral("ok")).toBool());
    QCOMPARE(registry.sequence(QStringLiteral("terminal.paste")), QString());
    QVERIFY(!registry.matches(QStringLiteral("terminal.paste"), Qt::Key_V,
                              int(Qt::ControlModifier | Qt::ShiftModifier)));
    QVERIFY(registry.terminalBindings().value(QStringLiteral("terminal.paste")).toString().isEmpty());
}

void ShortcutRegistryTest::resetsSingleAndAllBindings()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    Preferences state;
    QVERIFY(openAt(state, dir.filePath(QStringLiteral("state.db"))));
    ShortcutRegistry registry(&state.workspace);

    QVERIFY(registry.setBinding(QStringLiteral("terminal.find"), QStringLiteral("F3"))
                .value(QStringLiteral("ok")).toBool());
    QVERIFY(registry.setBinding(QStringLiteral("editor.find"), QStringLiteral("F4"))
                .value(QStringLiteral("ok")).toBool());
    registry.resetBinding(QStringLiteral("terminal.find"));
    QCOMPARE(registry.sequence(QStringLiteral("terminal.find")), QStringLiteral("Ctrl+Shift+F"));
    QCOMPARE(registry.sequence(QStringLiteral("editor.find")), QStringLiteral("F4"));
    registry.resetAll();
    QCOMPARE(registry.sequence(QStringLiteral("editor.find")), QStringLiteral("Ctrl+F"));
    QVERIFY(state.workspace.shortcutOverrides().isEmpty());

    // Rebinding to the default stores nothing.
    QVERIFY(registry.setBinding(QStringLiteral("editor.find"), QStringLiteral("Ctrl+F"))
                .value(QStringLiteral("ok")).toBool());
    QVERIFY(state.workspace.shortcutOverrides().isEmpty());
}

void ShortcutRegistryTest::ignoresCorruptStoredBindings()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("state.db"));
    {
        Preferences state;
        QVERIFY(openAt(state, path));
    }
    seedSetting(path, QStringLiteral("shortcuts.bindings"),
                QStringLiteral(R"({"terminal.find":"F","editor.find":"Ctrl+K, Ctrl+F","ghost":"Ctrl+Q"})"));
    Preferences state;
    QVERIFY(openAt(state, path));
    ShortcutRegistry registry(&state.workspace);
    QCOMPARE(registry.sequence(QStringLiteral("terminal.find")), QStringLiteral("Ctrl+Shift+F"));
    QCOMPARE(registry.sequence(QStringLiteral("editor.find")), QStringLiteral("Ctrl+F"));
}

void ShortcutRegistryTest::recordsChordsFromKeys()
{
    ShortcutRegistry registry(nullptr);
    QCOMPARE(registry.sequenceFromKey(Qt::Key_F, int(Qt::ControlModifier | Qt::ShiftModifier)),
             QStringLiteral("Ctrl+Shift+F"));
    QCOMPARE(registry.sequenceFromKey(Qt::Key_Shift, int(Qt::ShiftModifier)), QString());
    QCOMPARE(registry.sequenceFromKey(Qt::Key_F5, int(Qt::KeypadModifier)), QStringLiteral("F5"));
}

QTEST_MAIN(ShortcutRegistryTest)
#include "shortcut_registry_test.moc"
