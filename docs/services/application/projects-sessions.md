# 项目与会话

## 1. 所有者

| 对象 | 职责 |
|---|---|
| `ProjectWorkspaceRegistry` | 项目列表，存 `projects` 表（`path`、`name`、`last_opened`、`expanded`、`parent_path`）；模型行只有主项目，列出的工作树经 `worktrees` 角色挂在主项目下 |
| `OpenTargetRegistry`（QML `OpenTargets`） | 右键“打开方式”里的外部程序：文件资源管理器加探测到的编辑器 |
| `ProjectContext` | 每项目一个，惰性创建：`FileTreeModel`、共享 `GitRepositoryService`、按查看器瓦片隔离的 `DocumentManager` |
| `SessionModel` | 会话目录，存 `sessions` 表（id、标签、所属作用域、Shell 配置、活动状态）；作用域可为项目路径或 `remote:<hostId>`（见 [SSH 远程](../remote/ssh-remote.md)） |
| `TerminalManager` | 运行中的 `TerminalSession` 与进程树 |

- 移除项目只删记录，不动磁盘。
- `ProjectContext` 在展开文件树、切换项目或激活相关瓦片时才创建文件树并启动 Git 检测。

### 工作树子项

链接工作树（`.git` 是指向 `<主仓库>/.git/worktrees/<id>` 的文件）只在被要求时列出，不自动扫描显示：

- **何时列入**：在 SpanTui 中新建的工作树；从项目右键“工作树 ▸”里点选的已有工作树（列出主仓库 `.git/worktrees/*` 中存在且未列出的）；以“添加项目文件夹”或分支弹层打开的工作树文件夹。主项目已在列表中时挂到它下面（`parent_path` 记主项目路径）；主项目后加入时，已在顶层的它的工作树改挂到它下面。主项目不在列表中时工作树作为普通顶层项目。
- **侧栏排版**：项目行与工作树行都用文件夹图标，行右侧小字显示当前检出（分支名，分离 HEAD 时为 7 位提交号，读 HEAD 文件得出，见 `branch` 角色）。主项目展开后先列工作树，再列自己的会话（终端在最下）；工作树行以直角图标占主项目图标列表示缩进，它的会话与它的文件夹图标对齐。“没有会话”只在一行下面什么都没有时显示：有工作树的主项目不显示，空的工作树在自己下面显示。
- **移除**：工作树行“从列表移除”只删记录；“删除工作树…”执行 `git worktree remove`（见 [Git](../git/overview.md#工作树写操作)），成功后删记录，Git 拒绝（有改动或未跟踪文件）时弹确认，可改为 `--force`。两者都要求它的瓦片已关闭、会话已删除。移除主项目连同其下列出的工作树记录一起删除，不删任何文件。

### 文件夹缺失

项目文件夹被删除后条目仍保留，`available` 为假：侧栏变灰，悬停提示可右键移除，右键只剩“移除”。窗口重新激活、打开右键菜单与项目上下文的 Git 状态变化时 `refreshFolders()` 重查文件夹是否存在及其当前检出。移除缺失项目不要求先关闭工作：它的会话被删除、瓦片被关闭（进程已无工作目录）。仍存在的项目照旧要求先关闭全部瓦片和会话。

### 右键菜单

- **新建终端**：点击用默认 Shell；悬停展开全部可启动 Shell。
- **工作树 ▸**（Git 可用且为主仓库时）：新建工作树…，以及未列出的已有工作树。
- **打开方式 ▸**：文件资源管理器，以及 Windows 为常见源码扩展名（`kOpenWithProbeExtensions`）登记的推荐“打开方式”程序（`SHAssocEnumHandlers` + `ASSOC_FILTER_RECOMMENDED`），去掉系统目录下的程序与本程序，按可执行文件去重。不维护编辑器白名单：装了 Zed、VS Code 等并登记了源码文件关联即出现。探测在线程池中进行，菜单打开时重扫。编辑器以脱离进程 Job 的方式启动（`startUnaffiliatedProcess`），关闭 SpanTui 不会结束它。

### 新建工作树

首次新建时先问默认工作树文件夹（建议值为主项目旁的 `worktrees`），存为 `git.worktreeRoot`（[设置](settings-appearance.md)）。新建对话框填名称（同时是新分支名）、基于的分支（默认主项目当前分支）与位置；位置默认 `<工作树文件夹>\<主项目名>\<名称>`（路径字符换成 `-`），手动修改或选择后不再跟随名称。成功后列入主项目下并打开一个终端。

## 2. 会话生命周期

- **创建**：`SessionModel` 写记录，经 `WorkspaceController` 让 `TerminalManager` 建会话并启动进程。`shellProfile`（空串跟随默认）在首次创建终端时换成命令行，之后改配置不影响已有终端（见 [Shell](settings-appearance.md#shell)）。
- **同步**：`connectSessionTerminals`（`session_terminal_routing.cpp`）在目录或主机变化时重新同步；目录的活动会话即管理器的活动会话。主机列表读不到时保留的远程会话不启动，避免退化成本地 Shell。
- **关闭瓦片**：只移除视图，进程后台运行，会话仍在目录中。
- **删除会话**：`TerminalManager::closeSession()` 销毁会话 Job，终止整个进程树。
- **重启恢复**：重新加载会话列表。会话首次成为活动会话（`TerminalManager::setActiveSession`）或其终端视图首次可见时启动进程；已启动过的会话不自动重启，只经 `restartSession`。

## 3. 代码与测试

- `src/application/{project_workspace_registry, project_context, workspace_controller, session_terminal_routing, open_target_registry}.cpp`、`src/workspace/session_model.cpp`、`src/git/git_worktree_{layout, commands}.cpp`、`src/platform/windows/open_with_handlers.cpp`
- C++ 之间用结构体交换目录：`SessionModel::records()`/`load()` 收发 `SessionRecord`，`TerminalManager::synchronize()` 接收 `TerminalSessionSpec`；`QVariantMap` 只出现在给 QML 的接口上。
- 测试：`project_workspace_registry_test`（含文件夹缺失、工作树挂载与收养，用手写的工作树布局文件）、`workspace_controller_test`（含移除缺失项目丢弃其工作、工作树路径规则）、`git_worktree_test`、`workspace_session_routing_test`、`session_model_test`
