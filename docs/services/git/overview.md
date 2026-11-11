# Git 子系统

只读：工作区变更、Diff、提交历史、分支列表。写仓库在终端里完成；MERGE/REVERT/rebase 状态只展示，不在 GUI 中继续或中止。

## 1. 结构

- `GitRepositoryService`（`src/git/git_repository_service.*`）：仓库发现、刷新编排（`git_repository_refresh.cpp`：状态、统计、分支、历史、监视与轮询）、Diff 正文（`git_repository_diff.cpp`）、解析（`git_repository_parsers.cpp`）。
- `GitLaneScheduler`（`git_lane_scheduler.*`）：只管进程——车道、排队与顶替、输出上限、临时输出文件、busy 标志与 `taskStarted`/`taskFinished`/`busyChanged`。不认识仓库，只接收工作目录；错误经 `errorReported` 交给服务。
- 切换仓库：服务先 `invalidateResults()` 作废在跑结果，再 `cancelAll()`；作废进程退出时 `staleLaneFinished` 通知服务补做发现。

### 车道

| 车道 | 操作 | 说明 |
|---|---|---|
| `History` (0) | log、rev-list、branch | 分页读取（见 [历史](reads-history-diff.md#2-提交历史)），输出上限 128 MB |
| `Status` (1) | status、discover、base-content | 只有前台项目轮询（见 [状态](reads-history-diff.md#1-状态与轮询)）；未知操作也落在这里 |
| `Diff` (2) | 工作区统计、提交文件列表 | 只含元数据，不带 diff 行 |
| `Peek` (3) | `commit-preview` | 悬停卡片；静默，新顶旧；独立成道以免排在提交加载之后 |
| `DiffBody` (4) | `diff-body` | 单文件正文（见 [差异](reads-history-diff.md#3-差异)）；服务自己排队、一次一个——放在 Diff 车道上会被新顶旧，滚动时连续请求互相丢失 |

### 调度规则

- 每车道同时一个进程，其余按序排队。新请求只顶替**同种**请求（`supersedeKey`：一般为操作名；`discover` 与 `refresh` 同种；`commit-details`/`commit-files`/`commit-files-stats` 同种）：杀掉在跑的同种、原位替换排队的同种；不同种只排队，不互相中断。静默请求不顶替同种的可见请求。
- 输出越限即杀进程；进程抢在 kill 前退出时按留下的输出再判一次，越限仍报失败（`outputLimitExceeded`）。
- **进度配对**：每个可见进程以自己的操作名报开始与结束，被顶替的报 `Cancelled`；busy 只由置忙的操作清除，没有"清除当前操作"的入口。`busyChanged` 在任一车道的 busy 或操作名变化时发出，`historyViewLoading`/`commitViewLoading`/`changesViewLoading` 依赖它。

## 2. 进程与安全

- Git 进程继承应用 Job（`g_applicationJob`），随主程序退出终止。
- 只启动 `core::toolExecutable(ExternalTool::Git)` 的绝对路径，不把裸 `git` 交给 `CreateProcess`（它先搜当前目录，而资源管理器菜单以被打开的文件夹为当前目录启动 SpanTui）。查找顺序：用户指定 > PATH 绝对条目（含注册表持久值）> Git for Windows 标准目录。低于 2.16（porcelain v2、`--absolute-git-dir`、`--ignored=matching` 的下限）视为不可用。
- Git 不可用时 `ProjectWorkspaceRegistry::setGitEnabled(false)` 让每个服务的项目路径置空：不发现、不轮询、不启动进程；相关界面随 `available == false` 隐藏。可执行文件变化时先关再开。
- 参数只经 `QStringList`；路径置于 `--` 之后，只带引用的命令也以 `--` 结尾（避免分支名与目录同名时歧义）。
- `resolveWorktreePath`/`isSafeGitRef`/`isSafeGitObjectId` 拒绝越界路径与非法引用；对象 ID 接受 7–64 位十六进制（兼容 SHA-256）。
- 不截获、不缓存凭据；错误输出中远程 URL 的令牌脱敏。

## 3. 代码与测试

- `src/git/`：`git_repository_service`、`git_lane_scheduler`、`git_repository_refresh`、`git_repository_parsers`、`git_repository_diff`、`git_diff_store`
- 测试：`git_lane_scheduler_test`（排队、顶替、进度配对）、`git_repository_read_test`、`git_repository_concurrency_test`、`git_parsers_test`、`git_item_model_test`、`workspace_controller_test`（前台项目集合）

## 4. 本域改动完成标准

### Git 操作
- [ ] 仍只读，不引入 commit、add、push、checkout、reset 等写命令。
- [ ] 参数经 `QStringList`，用户可控路径在 `--` 之后并经 `resolveWorktreePath`，引用与对象名经 `isSafeGitRef`/`isSafeGitObjectId`。
- [ ] 归入正确车道；未知操作不落入写路径。
- [ ] 异步结果按代次丢弃陈旧响应，切换仓库或关闭项目后不回写。
- [ ] 进度信号只带操作名；到任务进度的映射在 `WorkspaceRuntime` 接线处补齐（见 [消息与任务](../application/messages-task-progress.md)）。

### 解析器
- [ ] `tests/git/` 用本地 bare 仓库夹具覆盖新格式（空仓库、合并提交、非 ASCII 路径）。
- [ ] 远程 URL 令牌仍脱敏。
