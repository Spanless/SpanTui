# 工作区数据库

`<root>\span_tui.db`，由 `SettingsStore::open()` 打开，工作区模式随即 `migrateWorkspaceDatabase`。

## 1. 连接

| 连接 | 模式 |
|---|---|
| `SettingsStore` | WAL，`busy_timeout=3000` |
| `MessageCenter`（工作区启动完成后） | 独立读写连接，`busy_timeout=3000` |
| 独立终端 | `QSQLITE_OPEN_READONLY` + `PRAGMA query_only=ON`，不开消息写连接 |

`-wal`/`-shm` 由 SQLite 管理，运行时不得手动删除。

## 2. 表

| 表 | 主键 | 字段 | 所有者 |
|---|---|---|---|
| `settings` | `key` | `value` | `SettingsStore`（见 [设置](../application/settings-appearance.md)） |
| `projects` | `id` 自增；`path` 唯一 | `name`、`last_opened`、`expanded`、`parent_path`（列在主项目下的工作树记主项目路径，顶层项目 `''`） | `ProjectWorkspaceRegistry` |
| `sessions` | `id` | `project_path`、`name`、`cwd`、`active`、`sort_order`、`shell_profile`（`''` 跟随默认） | `SessionModel` |
| `document_tabs` | (`owner_project`, `owner_tile`, `path`) | `name`、`cursor_position`、`sort_order`、`active`、`disk_mtime`、`utf8_bom`；不存内容 | 每个查看器瓦片的 `DocumentManager` |
| `messages` | `id` 自增 | `kind`、`title`、`body`、`created_at`、`read`；最多 200 条 | `MessageCenter` |
| `remote_hosts` | `id` | `name`、`host`、`port`、`user`、`identity_file`、`expanded`、`sort_order`、`password`、`key_passphrase`（DPAPI 密文） | `RemoteHostRegistry` |

## 3. 迁移（`WorkspaceDatabaseMigration`）

`src/core/workspace_database_migration.cpp` 是表结构的唯一所有者；其他模块只打开已迁移的库，不执行 `CREATE`/`ALTER`。

- 基线 `kWorkspaceDatabaseVersion`（`core_constants.h`）= 11：全部表与索引以 `create … if not exists` 定义在一份基线中。
- 版本不足时在单一事务中执行基线、`dropRetiredSchema()`（按 `pragma table_info` 删除旧库中只写不读的列与表）、v9 补列（`sessions.shell_profile`）、v10 设置键改名（`renameSettingKeys()`，如 `terminalCursorStyle` → `terminal.cursorStyle`；新键已存在时保留新键、删除旧键）与 v11 补列（`projects.parent_path`，旧项目全部为顶层），写入 `user_version`；任一步失败即回滚，`user_version` 不变。
- 支持 v5–v10 及新库；更早的库不再升级。
- 改结构：修改基线并提升版本号；已有库需要的列变更写成该版本的显式步骤，同样在事务内。

测试：`workspace_database_migration_test`（含未打开连接被拒、中途失败整体回滚）。
