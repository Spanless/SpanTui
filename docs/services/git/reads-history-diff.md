# Git 状态、历史与 Diff

## 1. 状态与轮询

- 所有命令用 `--porcelain` 或 `-z`，不解析本地化文本。
- status 带 `--untracked-files=all`（新目录里每个文件各一行）与 `--ignored=matching`（忽略目录只报一条；传统模式配合 `-uall` 会逐个列出）。
- 解析结果以相对路径推送给 `FileTreeModel` 显示徽标。
- **节奏**：默认 10 秒；`IdleGovernor` 判定空闲时 30 秒，且只刷新状态映射，不重算统计与 diff 行；恢复活跃立即 `scheduleRefresh`。
- **触发来源**：`.git` 内的 HEAD、index、refs 由 `QFileSystemWatcher` 监视，变化后合并刷新；工作区文件改动由 `DirectoryChangeWatcher`（`src/platform/windows/directory_change_watcher.*`，`ReadDirectoryChangesW` 递归监视仓库根，在 GUI 事件循环完成，不另开线程）标记"有改动"。轮询到点时只有标记为真才跑 `git status`，安静的仓库不启动 git 进程；状态刷新开始时清除标记，失败则重新标记。git 目录内的变化不计；通知溢出视为有改动；无法监视（目录不支持通知、根被删除）时退回每次轮询都刷新。后台项目停止监视，回到前台重新监视并完整刷新。
- **只有前台项目轮询**：前台 = 中央区瓦片所属项目 + 右栏绑定项目（`boundTerminalProjectPath`），由 `WorkspaceController` 算出、经 `ProjectWorkspaceRegistry` 对每个 `ProjectContext` `setForeground`。后台项目保留已读状态，停止轮询并忽略监视器触发，新建的后台上下文不发现仓库；回到前台立即完整刷新并恢复轮询。
- **普通目录**：`rev-parse` 报 "not a git repository" 是正常的不可用状态，不轮询；以 `QFileSystemWatcher` 监视项目根（`.git` 已存在时也监视它，应对与 `git init` 的竞态），出现 `.git` 再发现。其他错误（所有权存疑、损坏、无法运行）显示错误并在轮询时重试。

## 2. 提交历史

### 分页

- `git log <ref> --topo-order --max-count=500 --skip=N`（`git_constants.h` 的 `kGitHistoryPageSize`），后台线程 `parseHistoryPage` 解析后一次写入模型；`HistoryGraph` 保存最后一行之后的泳道与颜色，下一页接着画。
- **续页**：`GitLogView` 滚到底调 `loadMoreHistory()`，固定从首页的头提交哈希继续（不跟随分支名），分支在此期间移动也能接上。不足 500 条即完整，不再起进程。
- **引用变化**：只重读首页。已加载超过一页且旧头提交在新首页中时，把其上方的新提交 `prepend` 到顶部、首页覆盖的行换成新版本；下方保留的前提是新首页末行与旧模型对应行的图谱字段相同，且移动过的引用（附注标签按剥离后的提交）都不在首页以下。否则（含 amend、reset）清空重载。
- `GitItemModel::rowOfHash` 按哈希索引查行，模型变化后首次查找时重建。
- **代次**（`m_historyRequestGeneration`）：每次请求页、当前分支变化或重载都递增，迟到的进程与解析结果不符即丢弃。
- 当前分支无提交（`# branch.oid (initial)`）时历史为空且不报错，不执行注定失败的 `git log`，同样递增代次。

### 图谱

- 每条线段（`in`/`through`/`out`）带 `target`：该泳道通往的提交哈希，泳道按目标唯一，视图据此跨行追踪血统线。
- **汇合**：右侧分支先占住某提交的泳道、随后左侧提交的第一父也是它时，第一父线胜出——该提交移到左侧列并沿用其颜色，主线不横移、不变色。
- 泳道只在行下半段换列；泳道间距由已加载行最宽的 `laneCount` 统一决定（追加页时增量维护，顶部插入时重建），相邻行线段对齐。
- 选中提交沿第一父血统线加粗，其余淡化但仍可读（`Theme.fadeMd`/`fadeLg`）。筛选时图列收成单列只画节点。
- 同名远端分支指向同一提交时并入本地徽标；`<remote>/HEAD` 不显示。

### 悬浮卡

`CommitHoverCard` 在列表旁有空间的一侧弹出。历史行只有标题；正文、共同作者（`Co-Authored-By`）与改动合计在卡片显示后经 `requestCommitPreview` 在 Peek 车道获取（扫过的行不各起进程），`commitPreviewReady` 返回并缓存 256 条，切换仓库清空。指针可移到卡片上保持打开；离开、开右键菜单或点「显示提交详情」时关闭。

## 3. 差异

工作区变更（`commitChanges`）与提交差异（`commitDiffFiles`）是只含元数据的文件列表（路径、旧路径、状态、增删数、二进制）。`GitCommitDiffList` 用一个虚拟化列表，展开某行才取正文，以 `TextLinesView` 显示。

### 正文获取

- 展开且委托存活的行经 `requestDiffLines(scope, paths)` 请求（`scope` 为 `changes`/`commit`，同帧合并），到达后 `diffLinesChanged` 通知，行用 `diffLines(scope, path)` 取回。「全部展开」不预取。
- 在 `DiffBody` 车道静默执行，解析与未跟踪文件读取在后台线程（`parseDiffBodies`），回到 GUI 时按请求序号与列表 `epoch` 校验。同时一个请求，每进程至多 16 个路径（重命名连同旧路径）；两张列表轮流，各自新请求优先；排队超过 64 丢弃最旧并通知对应行。选中别的提交或切换仓库时清空该列表的队列与缓存。
- 快速拖动（一帧滚动超过半个视口）期间新出现的展开行只显示占位，停止约 `Theme.durFast` 后才取正文。

### 存储与上限

- `GitDiffBody`（每行 `{kind, oldLine, newLine, text}`），每列表一个 `GitDiffStore`，按估算字节 32 MiB LRU 淘汰。行持有自己的副本，淘汰不会让屏上 diff 变空。
- 单文件至多 5000 行、每行 1000 字符（截断加 `…`），增删数按完整 diff。一批输出超过 16 MiB 时拆成单文件重试，单文件仍超限标为「差异过大」。Git 失败的文件缓存错误，不反复重试。
- 未跟踪文件从磁盘读（超 2 MiB 为过大，含 NUL 为二进制）。分支无提交时工作区每个文件都按整份新增从磁盘读，不起 Git 进程。
- 每次状态刷新后，最近查看的 24 个工作区正文在后台重取，内容摘要不变则不通知；其余缓存丢弃并通知。
- numstat 不含未跟踪文件：统计刷新后在后台按正文规则数行数，再发布统计与合计（按序号与代次校验）。
- 未跟踪文件的行数与正文按（大小，修改时间）缓存（`UntrackedFileCache`，加锁）；正文合计超过 16 MiB 时只保留行数。切换仓库清空。

### 列表视图

- 平铺（文件名 + 淡色目录）或按目录分组（`DiffDirRow` 带组合计，可折叠）。不显示状态字母，状态由图标、`Theme.git*` 颜色、删除线与「← 旧名」表达。
- 统计列宽取全表最宽的统计（含分组合计）。
- 性能：`reuseItems`；折叠行只实例化 `DiffFileRow`，展开内容经 `Loader` 创建；行数据取自汇总时转换好的 JS 对象，不逐行 `itemAt`；批量变化每帧只汇总一次。

## 4. 代码

- `src/git/git_repository_service.cpp`、`git_repository_diff.cpp`、`git_diff_store.*`、`git_repository_parsers.cpp`
- QML：`qml/components/business/git/`（`GitPane`、`GitCommitDiffList`、`GitLogView`）、`editor/TextLinesView.qml`
