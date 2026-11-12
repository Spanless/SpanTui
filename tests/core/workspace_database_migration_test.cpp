#include "core/core_constants.h"
#include "core/workspace_database_migration.h"

#include <QSqlDatabase>
#include <QSqlQuery>
#include <QTemporaryDir>
#include <QtTest>

class WorkspaceDatabaseMigrationTest final : public QObject {
    Q_OBJECT

private slots:
    void createsBaselineSchemaIdempotently();
    void upgradesExistingTablesWithoutTouchingRows();
    void dropsRetiredColumnsFromVersionSix();
    void versionSevenGainsRemoteHosts();
    void versionNineRenamesSettingKeys();
    void refusesClosedDatabase();
    void failedUpgradeLeavesDatabaseUntouched();
};

namespace {

QStringList tableColumns(QSqlDatabase &database, const QString &table)
{
    QStringList columns;
    QSqlQuery query(database);
    if (query.exec(QStringLiteral("pragma table_info(%1)").arg(table))) {
        while (query.next()) {
            columns.push_back(query.value(1).toString());
        }
    }
    return columns;
}

// Every settings row as "key=value", ordered by key.
QStringList settingRows(QSqlDatabase &database)
{
    QStringList rows;
    QSqlQuery query(database);
    if (query.exec(QStringLiteral("select key, value from settings order by key"))) {
        while (query.next()) rows << query.value(0).toString() + QLatin1Char('=') + query.value(1).toString();
    }
    return rows;
}

int userVersion(QSqlDatabase &database)
{
    QSqlQuery query(database);
    return query.exec(QStringLiteral("pragma user_version")) && query.next() ? query.value(0).toInt() : -1;
}

} // namespace

void WorkspaceDatabaseMigrationTest::createsBaselineSchemaIdempotently()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString connection = QStringLiteral("workspace_migration_fresh");
    {
        QSqlDatabase database = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), connection);
        database.setDatabaseName(temporary.filePath(QStringLiteral("fresh.db")));
        QVERIFY(database.open());

        QString error;
        QVERIFY2(span::core::migrateWorkspaceDatabase(database, &error), qPrintable(error));
        QVERIFY2(span::core::migrateWorkspaceDatabase(database, &error), qPrintable(error));
        QCOMPARE(userVersion(database), span::core::kWorkspaceDatabaseVersion);

        QCOMPARE(tableColumns(database, QStringLiteral("settings")),
                 QStringList({QStringLiteral("key"), QStringLiteral("value")}));
        QCOMPARE(tableColumns(database, QStringLiteral("projects")),
                 QStringList({QStringLiteral("id"), QStringLiteral("path"), QStringLiteral("name"),
                              QStringLiteral("last_opened"), QStringLiteral("expanded")}));
        QCOMPARE(tableColumns(database, QStringLiteral("sessions")),
                 QStringList({QStringLiteral("id"), QStringLiteral("project_path"), QStringLiteral("name"),
                              QStringLiteral("cwd"), QStringLiteral("active"), QStringLiteral("sort_order"),
                              QStringLiteral("shell_profile")}));
        QCOMPARE(
            tableColumns(database, QStringLiteral("document_tabs")),
            QStringList({QStringLiteral("path"), QStringLiteral("name"), QStringLiteral("cursor_position"),
                         QStringLiteral("sort_order"), QStringLiteral("active"), QStringLiteral("disk_mtime"),
                         QStringLiteral("utf8_bom"), QStringLiteral("owner_project"), QStringLiteral("owner_tile")}));
        QVERIFY(tableColumns(database, QStringLiteral("file_operations")).isEmpty());
        QCOMPARE(tableColumns(database, QStringLiteral("messages")),
                 QStringList({QStringLiteral("id"), QStringLiteral("kind"), QStringLiteral("title"),
                              QStringLiteral("body"), QStringLiteral("created_at"), QStringLiteral("read")}));
        QCOMPARE(
            tableColumns(database, QStringLiteral("remote_hosts")),
            QStringList({QStringLiteral("id"), QStringLiteral("name"), QStringLiteral("host"), QStringLiteral("port"),
                         QStringLiteral("user"), QStringLiteral("identity_file"), QStringLiteral("password"),
                         QStringLiteral("key_passphrase"), QStringLiteral("expanded"), QStringLiteral("sort_order")}));
        database.close();
    }
    QSqlDatabase::removeDatabase(connection);
}

void WorkspaceDatabaseMigrationTest::upgradesExistingTablesWithoutTouchingRows()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString connection = QStringLiteral("workspace_migration_upgrade");
    {
        QSqlDatabase database = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), connection);
        database.setDatabaseName(temporary.filePath(QStringLiteral("existing.db")));
        QVERIFY(database.open());
        QSqlQuery query(database);
        // A version-5 database: settings and sessions exist, document_tabs
        // was never created because no document had been opened.
        QVERIFY(query.exec(QStringLiteral("create table settings (key text primary key, value text not null)")));
        QVERIFY(query.exec(QStringLiteral("insert into settings values('themeMode', 'dark')")));
        QVERIFY(
            query.exec(QStringLiteral("create table sessions (id text primary key, project_path text not null "
                                      "default '', name text not null, cwd text not null, active integer default 0, "
                                      "sort_order integer default 0)")));
        QVERIFY(query.exec(QStringLiteral("insert into sessions values('s-1', 'C:/work', 'shell', 'C:/work', 1, 0)")));
        QVERIFY(query.exec(QStringLiteral("pragma user_version = 5")));

        QString error;
        QVERIFY2(span::core::migrateWorkspaceDatabase(database, &error), qPrintable(error));
        QCOMPARE(userVersion(database), span::core::kWorkspaceDatabaseVersion);
        QVERIFY(!tableColumns(database, QStringLiteral("document_tabs")).isEmpty());
        QVERIFY(!tableColumns(database, QStringLiteral("remote_hosts")).isEmpty());

        QVERIFY(query.exec(QStringLiteral("select value from settings where key='themeMode'")));
        QVERIFY(query.next());
        QCOMPARE(query.value(0).toString(), QStringLiteral("dark"));
        QVERIFY(query.exec(QStringLiteral("select id, cwd, shell_profile from sessions")));
        QVERIFY(query.next());
        QCOMPARE(query.value(0).toString(), QStringLiteral("s-1"));
        QCOMPARE(query.value(1).toString(), QStringLiteral("C:/work"));
        // v9: existing sessions follow the default shell profile.
        QCOMPARE(query.value(2).toString(), QString());
        QVERIFY(!query.next());
        database.close();
    }
    QSqlDatabase::removeDatabase(connection);
}

void WorkspaceDatabaseMigrationTest::dropsRetiredColumnsFromVersionSix()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString connection = QStringLiteral("workspace_migration_v6");
    {
        QSqlDatabase database = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), connection);
        database.setDatabaseName(temporary.filePath(QStringLiteral("v6.db")));
        QVERIFY(database.open());
        QSqlQuery query(database);
        QVERIFY(query.exec(QStringLiteral("create table projects (id integer primary key autoincrement, "
                                          "path text unique not null, name text not null, active integer default 0, "
                                          "last_opened integer default 0, expanded integer default 1, "
                                          "run_command text default '', run_cwd text default '')")));
        QVERIFY(query.exec(QStringLiteral("insert into projects(path,name,active,expanded,run_command) "
                                          "values('C:/work','work',1,0,'run.bat')")));
        QVERIFY(query.exec(
            QStringLiteral("create table file_operations (id integer primary key autoincrement, "
                           "op text not null, source_path text, target_path text, created_at text not null)")));
        QVERIFY(query.exec(QStringLiteral("create table settings (key text primary key, value text not null)")));
        QVERIFY(
            query.exec(QStringLiteral("insert into settings values('git.favorites', '{}'), ('themeMode', 'dark')")));
        QVERIFY(query.exec(QStringLiteral("pragma user_version = 6")));

        QString error;
        QVERIFY2(span::core::migrateWorkspaceDatabase(database, &error), qPrintable(error));
        QCOMPARE(userVersion(database), span::core::kWorkspaceDatabaseVersion);
        QCOMPARE(tableColumns(database, QStringLiteral("projects")),
                 QStringList({QStringLiteral("id"), QStringLiteral("path"), QStringLiteral("name"),
                              QStringLiteral("last_opened"), QStringLiteral("expanded")}));
        QVERIFY(tableColumns(database, QStringLiteral("file_operations")).isEmpty());
        QVERIFY(query.exec(QStringLiteral("select key from settings")));
        QVERIFY(query.next());
        QCOMPARE(query.value(0).toString(), QStringLiteral("themeMode"));
        QVERIFY(!query.next());
        QVERIFY(query.exec(QStringLiteral("select path, expanded from projects")));
        QVERIFY(query.next());
        QCOMPARE(query.value(0).toString(), QStringLiteral("C:/work"));
        QCOMPARE(query.value(1).toInt(), 0);
        database.close();
    }
    QSqlDatabase::removeDatabase(connection);
}

void WorkspaceDatabaseMigrationTest::versionSevenGainsRemoteHosts()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString connection = QStringLiteral("workspace_migration_v7");
    {
        QSqlDatabase database = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), connection);
        database.setDatabaseName(temporary.filePath(QStringLiteral("v7.db")));
        QVERIFY(database.open());
        QSqlQuery query(database);
        QVERIFY(query.exec(QStringLiteral("create table settings (key text primary key, value text not null)")));
        QVERIFY(query.exec(QStringLiteral("insert into settings values('themeMode', 'light')")));
        QVERIFY(query.exec(QStringLiteral("pragma user_version = 7")));

        QString error;
        QVERIFY2(span::core::migrateWorkspaceDatabase(database, &error), qPrintable(error));
        QCOMPARE(userVersion(database), span::core::kWorkspaceDatabaseVersion);
        QVERIFY(
            query.exec(QStringLiteral("insert into remote_hosts(id, name, host) values('h', 'box', 'example.com')")));
        QVERIFY(query.exec(QStringLiteral("select port, user, expanded from remote_hosts where id='h'")));
        QVERIFY(query.next());
        QCOMPARE(query.value(0).toInt(), 22);
        QCOMPARE(query.value(1).toString(), QString());
        QCOMPARE(query.value(2).toInt(), 1);
        QVERIFY(query.exec(QStringLiteral("select value from settings")));
        QVERIFY(query.next());
        QCOMPARE(query.value(0).toString(), QStringLiteral("light"));
        database.close();
    }
    QSqlDatabase::removeDatabase(connection);
}

void WorkspaceDatabaseMigrationTest::versionNineRenamesSettingKeys()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString connection = QStringLiteral("workspace_migration_v9");
    {
        QSqlDatabase database = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), connection);
        database.setDatabaseName(temporary.filePath(QStringLiteral("v9.db")));
        QVERIFY(database.open());
        QSqlQuery query(database);
        QVERIFY(query.exec(QStringLiteral("create table settings (key text primary key, value text not null)")));
        // windowX already has its new key: the new value wins and the old row
        // goes. Keys outside the rename list stay as they are.
        QVERIFY(query.exec(QStringLiteral("insert into settings values('terminalCursorStyle', 'bar'), "
                                          "('scriptRunMode', 'independentWindow'), ('windowX', '10'), "
                                          "('window.x', '20'), ('themeMode', 'dark')")));
        QVERIFY(query.exec(QStringLiteral("pragma user_version = 9")));

        QString error;
        QVERIFY2(span::core::migrateWorkspaceDatabase(database, &error), qPrintable(error));
        QCOMPARE(userVersion(database), span::core::kWorkspaceDatabaseVersion);
        QCOMPARE(
            settingRows(database),
            (QStringList{QStringLiteral("terminal.cursorStyle=bar"), QStringLiteral("themeMode=dark"),
                         QStringLiteral("window.x=20"), QStringLiteral("workspace.scriptRunMode=independentWindow")}));
        database.close();
    }
    QSqlDatabase::removeDatabase(connection);
}

void WorkspaceDatabaseMigrationTest::refusesClosedDatabase()
{
    const QString connection = QStringLiteral("workspace_migration_closed");
    {
        QSqlDatabase database = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), connection);
        QString error;
        QVERIFY(!span::core::migrateWorkspaceDatabase(database, &error));
        QVERIFY(error.contains(QStringLiteral("not open")));
    }
    QSqlDatabase::removeDatabase(connection);
}

void WorkspaceDatabaseMigrationTest::failedUpgradeLeavesDatabaseUntouched()
{
    // SQLite cannot drop an indexed column: the v7 cleanup fails after the
    // baseline already ran, and the whole upgrade must roll back.
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString connection = QStringLiteral("workspace_migration_rollback");
    {
        QSqlDatabase database = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), connection);
        database.setDatabaseName(temporary.filePath(QStringLiteral("v6.db")));
        QVERIFY(database.open());
        QSqlQuery query(database);
        QVERIFY(query.exec(QStringLiteral("create table projects (id integer primary key autoincrement, "
                                          "path text unique not null, name text not null, active integer default 0)")));
        QVERIFY(query.exec(QStringLiteral("create index projects_active on projects(active)")));
        QVERIFY(query.exec(QStringLiteral("pragma user_version = 6")));

        QString error;
        QVERIFY(!span::core::migrateWorkspaceDatabase(database, &error));
        QVERIFY(!error.isEmpty());
        QCOMPARE(userVersion(database), 6);
        QVERIFY(tableColumns(database, QStringLiteral("projects")).contains(QStringLiteral("active")));
        QVERIFY(tableColumns(database, QStringLiteral("remote_hosts")).isEmpty());
        database.close();
    }
    QSqlDatabase::removeDatabase(connection);
}

QTEST_MAIN(WorkspaceDatabaseMigrationTest)

#include "workspace_database_migration_test.moc"
