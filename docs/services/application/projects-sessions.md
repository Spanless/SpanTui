# 项目与会话

## 1. 所有者

| 对象 | 职责 |
|---|---|
| `ProjectWorkspaceRegistry` | 项目列表，存 `projects` 表（`path`、`name`、`last_opened`、`expanded`） |
| `ProjectContext` | 每项目一个，惰性创建：`FileTreeModel`、共享 `GitRepositoryService`、按查看器瓦片隔离的 `DocumentManager` |
| `SessionModel` | 会话目录，存 `sessions` 表（id、标签、所属作用域、Shell 配置、活动状态）；作用域可为项目路径或 `remote:<hostId>`（见 [SSH 远程](../remote/ssh-remote.md)） |
| `TerminalManager` | 运行中的 `TerminalSession` 与进程树 |

- 移除项目只删记录，不动磁盘。
- `ProjectContext` 在展开文件树、切换项目或激活相关瓦片时才创建文件树并启动 Git 检测。

## 2. 会话生命周期

- **创建**：`SessionModel` 写记录，经 `WorkspaceController` 让 `TerminalManager` 建会话并启动进程。`shellProfile`（空串跟随默认）在首次创建终端时换成命令行，之后改配置不影响已有终端（见 [Shell](settings-appearance.md#shell)）。
- **同步**：`connectSessionTerminals`（`session_terminal_routing.cpp`）在目录或主机变化时重新同步；目录的活动会话即管理器的活动会话。主机列表读不到时保留的远程会话不启动，避免退化成本地 Shell。
- **关闭瓦片**：只移除视图，进程后台运行，会话仍在目录中。
- **删除会话**：`TerminalManager::closeSession()` 销毁会话 Job，终止整个进程树。
- **重启恢复**：重新加载会话列表。会话首次成为活动会话（`TerminalManager::setActiveSession`）或其终端视图首次可见时启动进程；已启动过的会话不自动重启，只经 `restartSession`。

## 3. 代码与测试

- `src/application/{project_workspace_registry, project_context, workspace_controller, session_terminal_routing}.cpp`、`src/workspace/session_model.cpp`
- C++ 之间用结构体交换目录：`SessionModel::records()`/`load()` 收发 `SessionRecord`，`TerminalManager::synchronize()` 接收 `TerminalSessionSpec`；`QVariantMap` 只出现在给 QML 的接口上。
- 测试：`project_workspace_registry_test`、`workspace_session_routing_test`、`session_model_test`
