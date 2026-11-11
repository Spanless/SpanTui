#include "core/workspace_database_migration.h"

#include "core/core_constants.h"

#include <QSqlDatabase>
#include <QSqlError>
#include <QPair>
#include <QSqlQuery>
#include <QStringList>

namespace span::core {
namespace {

// The baseline creates every table the workspace owns in its current shape.
// Existing v5/v6 databases already hold some of these tables in an older
// shape; dropRetiredSchema() brings them to the same result. v7 databases
// only gain remote_hosts; v8 databases gain sessions.shell_profile.
const QStringList &baselineSchema()
{
    static const QStringList statements{
        QStringLiteral("create table if not exists settings ("
                       "key text primary key, value text not null)"),
        QStringLiteral("create table if not exists projects ("
                       "id integer primary key autoincrement, path text unique not null, "
                       "name text not null, last_opened integer default 0, "
                       "expanded integer default 1)"),
        QStringLiteral("create table if not exists sessions ("
                       "id text primary key, project_path text not null default '', "
                       "name text not null, cwd text not null, active integer default 0, "
                       "sort_order integer default 0, shell_profile text not null default '')"),
        QStringLiteral("create table if not exists document_tabs ("
                       "path text not null, name text not null, "
                       "cursor_position integer not null, "
                       "sort_order integer not null, active integer not null, "
                       "disk_mtime integer not null, utf8_bom integer not null, "
                       "owner_project text not null default '', "
                       "owner_tile text not null default '', "
                       "primary key(owner_project, owner_tile, path))"),
        QStringLiteral("create index if not exists document_tabs_owner "
                       "on document_tabs(owner_project, owner_tile, sort_order)"),
        // Saved SSH hosts (v8). password/key_passphrase hold Windows DPAPI
        // blobs for the current user, never plain text.
        QStringLiteral("create table if not exists remote_hosts ("
                       "id text primary key, name text not null, host text not null, "
                       "port integer not null default 22, user text not null default '', "
                       "identity_file text not null default '', password blob, "
                       "key_passphrase blob, expanded integer not null default 1, "
                       "sort_order integer not null default 0)"),
        QStringLiteral("create table if not exists messages ("
                       "id integer primary key autoincrement, "
                       "kind text not null, title text not null, body text not null, "
                       "created_at integer not null, read integer not null default 0)"),
    };
    return statements;
}

bool hasColumn(QSqlQuery &query, const QString &table, const QString &column)
{
    if (!query.exec(QStringLiteral("pragma table_info(%1)").arg(table))) {
        return false;
    }
    while (query.next()) {
        if (query.value(1).toString() == column) {
            return true;
        }
    }
    return false;
}

// v7: columns, tables and settings that were written but never read.
bool dropRetiredSchema(QSqlQuery &query)
{
    static const QList<QPair<QString, QString>> columns{
        {QStringLiteral("projects"), QStringLiteral("active")},
        {QStringLiteral("projects"), QStringLiteral("run_command")},
        {QStringLiteral("projects"), QStringLiteral("run_cwd")},
        {QStringLiteral("document_tabs"), QStringLiteral("content")},
        {QStringLiteral("document_tabs"), QStringLiteral("scroll_y")},
        {QStringLiteral("document_tabs"), QStringLiteral("missing")},
    };
    for (const auto &[table, column] : columns) {
        if (hasColumn(query, table, column)
            && !query.exec(QStringLiteral("alter table %1 drop column %2").arg(table, column))) {
            return false;
        }
    }
    return query.exec(QStringLiteral("drop table if exists file_operations"))
        && query.exec(QStringLiteral("delete from settings where key='git.favorites'"));
}

// v9: the shell profile each terminal session starts ('' = the default).
bool addSessionShellProfile(QSqlQuery &query)
{
    return hasColumn(query, QStringLiteral("sessions"), QStringLiteral("shell_profile"))
        || query.exec(QStringLiteral("alter table sessions add column shell_profile text not null default ''"));
}

// v10: settings keys take a `domain.` prefix. A new key already present wins.
bool renameSettingKeys(QSqlQuery &query)
{
    static const QList<QPair<QString, QString>> keys{
        {QStringLiteral("terminalCursorStyle"), QStringLiteral("terminal.cursorStyle")},
        {QStringLiteral("terminalRefreshRate"), QStringLiteral("terminal.refreshRate")},
        {QStringLiteral("shellProfiles"), QStringLiteral("terminal.shellProfiles")},
        {QStringLiteral("scriptRunMode"), QStringLiteral("workspace.scriptRunMode")},
        {QStringLiteral("terminateTerminalOnTileClose"), QStringLiteral("workspace.terminateTerminalOnTileClose")},
        {QStringLiteral("workspaceLayoutJson"), QStringLiteral("workspace.layoutJson")},
        {QStringLiteral("performanceMonitorIntervalSeconds"), QStringLiteral("monitor.intervalSeconds")},
        {QStringLiteral("shortcutBindings"), QStringLiteral("shortcuts.bindings")},
        {QStringLiteral("toolPaths"), QStringLiteral("tools.paths")},
        {QStringLiteral("windowX"), QStringLiteral("window.x")},
        {QStringLiteral("windowY"), QStringLiteral("window.y")},
        {QStringLiteral("windowWidth"), QStringLiteral("window.width")},
        {QStringLiteral("windowHeight"), QStringLiteral("window.height")},
        {QStringLiteral("windowMaximized"), QStringLiteral("window.maximized")},
    };
    for (const auto &[from, to] : keys) {
        query.prepare(QStringLiteral("update settings set key = ? where key = ? "
                                     "and not exists (select 1 from settings where key = ?)"));
        query.addBindValue(to);
        query.addBindValue(from);
        query.addBindValue(to);
        if (!query.exec()) {
            return false;
        }
        query.prepare(QStringLiteral("delete from settings where key = ?"));
        query.addBindValue(from);
        if (!query.exec()) {
            return false;
        }
    }
    return true;
}

bool fail(QSqlDatabase &database, QString *errorMessage, const QString &message)
{
    database.rollback();
    if (errorMessage) {
        *errorMessage = message;
    }
    return false;
}

} // namespace

bool migrateWorkspaceDatabase(QSqlDatabase &database, QString *errorMessage)
{
    if (!database.isOpen()) {
        if (errorMessage) {
            *errorMessage = QStringLiteral("Workspace database is not open.");
        }
        return false;
    }

    QSqlQuery query(database);
    if (!query.exec(QStringLiteral("pragma user_version")) || !query.next()) {
        if (errorMessage) {
            *errorMessage = query.lastError().text();
        }
        return false;
    }
    if (query.value(0).toInt() >= kWorkspaceDatabaseVersion) {
        return true;
    }

    if (!database.transaction()) {
        if (errorMessage) {
            *errorMessage = database.lastError().text();
        }
        return false;
    }
    for (const QString &statement : baselineSchema()) {
        if (!query.exec(statement)) {
            return fail(database, errorMessage, query.lastError().text());
        }
    }
    if (!dropRetiredSchema(query) || !addSessionShellProfile(query) || !renameSettingKeys(query)) {
        return fail(database, errorMessage, query.lastError().text());
    }
    if (!query.exec(QStringLiteral("pragma user_version = %1").arg(kWorkspaceDatabaseVersion)) || !database.commit()) {
        return fail(database, errorMessage,
                    query.lastError().text().isEmpty() ? database.lastError().text() : query.lastError().text());
    }
    return true;
}

} // namespace span::core
