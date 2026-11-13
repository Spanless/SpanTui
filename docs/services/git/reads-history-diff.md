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

## 4. 引用与工作树

### 读取

- `branches` 模型：`git for-each-ref` 读本地、远程分支与标签（`kind` 为 `local`/`remote`/`tag`）。本地分支的上游状态来自 `%(upstream:short)` 与 `%(upstream:track)`：`hasUpstream`（配置了上游）、`upstreamGone`（配置了但跟踪引用已不存在，track 为 `[gone]`）、`ahead`（本地有而上游没有，即未推送）、`behind`（上游有而本地没有，即未拉取）。
  - **只比本地已有的远程跟踪引用**：不自动 `fetch`（fetch 会改远程跟踪引用，属于写），所以"落后"反映的是上次抓取（终端里的 `git fetch`/`pull`、或 IDE 的后台抓取）时远端的样子；远端之后的新提交要等下次抓取才计入。
- `worktrees` 模型：`git worktree list --porcelain`（不用 `-z`，它要求 git 2.36，支持下限是 2.16），`parseWorktrees` 解析。字段：`path`（`cleanPath`，正斜杠，也是行标识）、`displayPath`（本机分隔符，用于提示与复制）、`relativePath`（相对主工作树的本机分隔符路径，如 `..\worktrees\topic`；主工作树为空，跨盘时 Qt 给出绝对路径）、`name`（目录名）、`branch`（去掉 `refs/heads/`，游离或裸库为空）、`head`（7 位）/`headId`（完整）、`detached`、`bare`、`locked`/`lockReason`、`prunable`/`prunableReason`、`main`（git 总是先列主工作树）、`current`（`path` 与本服务的仓库根大小写不敏感相等）。
- 两者都在 `History` 车道上静默读取（`worktrees` 排在 `branches` 之后，分支列表不等工作树）。每次带分支的刷新（发现仓库、`HEAD`/refs 变化）都读一次工作树；`<common>/worktrees` 目录没有监视，所以在终端里 `git worktree add` 之后要点刷新按钮或等下一次分支刷新。

### 手动刷新

`refreshRefs()` 重读分支、标签与工作树，`refsRefreshing` 在两个读取都落地之前为真（`refsRefreshingChanged`）。

- 与自动读取共用同一对读取：自动读取顶替了手动读取时由它落地并结束刷新，不会卡在进行中。
- 校验：每次读取记下 `m_refsEpoch`；切换仓库（`clearRepository`）递增它并清空两个模型、结束进行中状态，之后到达的旧仓库结果因代次不符被丢弃。
- 失败时（仅手动刷新或可见读取）错误进入 `errorMessage`，进行中状态照常结束。
- 无可用仓库时调用为空操作。
- 重读的工作树列表落地后再探测一轮改动（见下节）。

### 工作树改动探测

`probeWorktreeChanges()` 给每个工作树标出"有未提交改动"，结果在 `worktreeChanges`（`path` → `bool`，`worktreeChangesChanged`）；没有条目表示未知。

- **异步**：调用立即返回；`Probe` 车道上一次一个工作树跑 `status --porcelain -z --untracked-files=no --ignore-submodules=all`，每个落地即更新一条，浮窗先画出再逐行补上标记。
- **口径**：只看已跟踪文件的暂存与未暂存改动，不算未跟踪文件——列未跟踪文件要遍历整棵树，代价最大；子模块不计。`GIT_OPTIONAL_LOCKS=0` 让探测不写对方的索引。
- **上限**：每轮最多 `kGitWorktreeProbeLimit`（16）个工作树，按列表顺序；跳过裸库与目录缺失的；只有主工作树时不探测（它的改动在 Git 面板里）。本轮不再检查的条目（已不在列表、或排在上限之后）删除，仍检查的旧结果保留到新结果落地。
- **代次**：每轮递增 `m_worktreeProbeGeneration`，新一轮同时顶替车道上在跑的探测；切换仓库递增代次并清空结果与队列。代次不符的完成回调直接丢弃。
- **触发**：分支浮窗每次打开时调用；工作树列表还没读到时先记下，列表落地后开始；手动刷新也会重探。失败的检查删除该条（回到未知），不报错。

### 分支浮窗

终端头部的分支按钮（`ChromeButton`，图标 `git-branch` 在分支名之前，图标与名称同属一个点击目标，悬停与按下底色覆盖整体，浮窗打开时 `selected`）打开 `BranchPopup`（`AnchorPanel`）。按钮后的 `+N -M` 只在有改动时出现，前面不再单放提交图标。窄宽度下分支名先省略、再只留图标（见 [工作区组合 · 终端头部](../../ui/layout/workspace-composition.md#终端头部)）。

- **表面**：用 `AppPopup` 自带的 `PopupSurface` 背景，浮窗本身不裁剪。旧实现在浮窗上设 `clip: true` 并在内容里再画一层 `PopupSurface`，表面的投影与抗锯齿轮廓画在自身矩形之外，被裁剪后四角留下灰色楔形、投影被切平；内容也直接顶到圆角。现在列表视口下缘离浮窗底边 `spaceSm`，滚动条端点按 `radiusLg` 内缩，行高亮因此不会碰到圆角。
- **尺寸**：宽 `dialogMd`；高随内容，上限为 `dialogMd` 与窗口高度减边距的较小者，列表区至少三行高。
- **只读**：浮窗里没有任何改仓库的操作（不切换、不拉取、不抓取）；这些在终端里做。打开时调用 `probeWorktreeChanges()`。
- **顶栏**：筛选框（匹配分支名、标签名；本地分支还匹配检出它的工作树的目录名与路径，其他工作树匹配目录名、分支与路径）右侧是刷新按钮；刷新中按钮换成 `LoadingSpinner`，至少显示一个 `durSlow`，避免毫秒级读取闪一下。
- **以分支为中心**：工作树挂在它检出的本地分支行上，不再单列一个与分支重复的"工作树"区。只有主工作树时它就是仓库本身，不画工作树信息。
- **分区**（顺序固定：本地、其他工作树、远程、标签）：每区一个 `RefsSectionHeader`（折叠箭头 + 次级小字标题，不显示行数——表头只是轻量标注，层级低于它引出的行），没有行的分区不画。「其他工作树」收没有对应本地分支行的工作树：游离 HEAD、裸库、或所在分支不在引用列表里。筛选时各区强制展开，全无匹配时显示空状态；没有任何分支（首次提交前）时显示另一条空状态。远程分支按远程分组（按首次出现顺序，同一远程不会重复出现表头）。
- **行**：每行一行、等高，不叠第二行与框形徽章；尾部信息右对齐，用标记而非文字，名称超长时尾部省略。文字说明只在悬停提示里。
- **本地分支行**：当前分支用选中底色、强调色图标与加粗名称。名称之后依次是：
  - 上游状态：有上游时 `↑N`（未推送，`gitAdded` 色）`↓M`（未拉取，`gitModified` 色），等宽小字，为 0 的不显示；没有上游显示灰色 `arrow-up` 图标，上游已删除显示警告色 `alert` 图标。
  - 工作树标记（被某个工作树检出时）：只有文件夹图标，有未提交改动时跟一个 `gitModified` 色圆点；当前所在的工作树用强调色，目录缺失用危险色，其余灰色。不显示目录名与 `relativePath`（链接工作树的相对路径多是 `..\..\` 开头的长串），完整路径在悬停提示里。工作树的领先/落后就是该分支的。
  - 悬停提示：上游状态的完整说明（含"以上次抓取为准，浮窗不抓取"），被工作树检出时再加检出位置的完整路径、主工作树、锁定（及原因）、目录缺失与未提交改动。
- **其他工作树行**：文件夹图标 + 目录名，尾部灰字为游离的短哈希、`bare` 或分支名（最多占行宽一半），再跟未提交圆点。
- **颜色与提示**：本地强调色、远程蓝、标签紫，与 `GitRefBadge` 一致。列表只有一个共享 `Tip`，由悬停的行发布锚点，右键菜单打开时隐藏。提示显示在浮窗左侧、与悬停行顶边对齐：放在行下方会盖住后面几行，且 `ToolTip` 会吞掉落在它范围内的点击。
- **左键**（都不改仓库）：
  - 本地分支被另一个工作树检出（非当前、目录存在、不是裸库）→ 作为项目打开那个工作树：`WorkspaceController.openProjectFolder(path)`（未注册则添加为项目；显示它上次聚焦的会话，没有就新建终端）。其余本地分支 → 菜单。
  - 其他工作树：非当前且可打开 → 同上；否则菜单。
  - 远程分支、标签：菜单。
- **右键菜单**（`AppContextMenu`）：
  - 本地分支：被工作树检出时先列工作树操作——作为项目打开（当前工作树禁用）、在新终端中打开（`openProjectFolder(path, true)`，总是新建终端）、在文件资源管理器中打开、复制路径；然后复制分支名、复制提交哈希。
  - 其他工作树：同一组工作树操作，加复制提交哈希（`headId`）。
  - 远程分支：复制分支名、复制提交哈希。
  - 标签：复制标签名、复制提交哈希（附注标签取它指向的提交 `commitId`）。
  - 目录缺失或裸库时"打开"类操作禁用。
- **不做**：查看某分支的历史、与当前分支对比——Git 历史视图只显示当前分支（`loadHistory` 用 `m_currentBranch`），没有切换引用的入口。
- **关闭**：复制、打开工作树、在资源管理器中显示之后 `GitRefsTree.dismissRequested` 关闭浮窗。

## 5. 代码

- `src/git/git_repository_service.cpp`、`git_repository_refresh.cpp`（含工作树探测）、`git_repository_diff.cpp`、`git_diff_store.*`、`git_repository_parsers.cpp`
- QML：`qml/components/business/git/`（`GitPane`、`GitCommitDiffList`、`GitLogView`、`BranchPopup`、`GitRefsTree`、`RefsSectionHeader`）、`editor/TextLinesView.qml`；头部分支按钮在 `workspace/ContentContainer.qml`
