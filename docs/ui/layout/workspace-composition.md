# 工作区组合

主窗口 = 标题栏（`WindowControls`）+ `WorkspaceCanvas`。设置不是瓦片：`window.page === "settings"` 时 `Main.qml` 的 `settingsPageLoader` 实例化 `Settings` 并 `open()`（`AppPopup` 模态，遮罩覆盖整窗）；关闭经 `popupClosed` 复位 `page === "workspace"`。

## 1. 区域

- **统一顶带（无独立空标题栏）**：`WorkspaceCanvas` 从 `y = 0` 起铺满整窗；`Main.qml` 的 `titleBar` 是叠加在顶部同高带（`topBarHeight = max(Theme.paneHeaderH, macOS 红绿灯中线×2 或 Theme.titlebarH)`）上的透明窗口铬层，不把画布往下推，仅承载窗口级按钮：左侧的 `SidebarToggles`（`panel-left`/`panel-right`，左栏停靠时贴左栏顶带右缘，折叠时紧贴红绿灯右侧或窗口左缘 `spaceMd`）与 Windows 右端的 `WindowControls`（macOS 为系统红绿灯，空 `NSWindowToolbarStyleUnifiedCompact` 使红绿灯自然居中于约 38px 顶带中线）。左栏、中心首行瓦片与右栏各自绘制等高顶带与底发丝线，在 `y = topBarHeight` 连成一整条横向水平线；左栏或右栏折叠时，中心首行边缘瓦片的 `PaneHeader` 通过 `leadingInset`/`trailingInset` 自动让出窗口铬层空间。只有中心瓦片 `PaneHeader.interactiveContent`、`SidebarToggles`、`WindowControls` 与右栏 `pageTabs` 注册为标题带交互项，左栏与右栏顶带空白区始终可拖动窗口。
- **左栏**：顶带（macOS 左侧留给系统红绿灯，Windows 左侧显示应用名，右侧叠放 `SidebarToggles`）加底发丝线；其下为「本地」节（项目与会话；无项目时显示只占自身高度的空态，不自动弹文件夹选择器；项目/工作树行悬停时分支位置切为 `+` 快捷新建终端，无会话时不占空行）与「远程」节（SSH 主机与会话，最多占栏高 40%，双击主机新开终端；主机列表为空时隐藏标题栏 `+`，行内「添加 SSH 主机」采用与列表行等宽的虚线描边空态按钮）。ssh 不可用时隐藏「远程」节。底部固定单行应用底栏（上方按需显示运行中的任务行）：左端为「消息」入口（含未读角标），右端依次为「更新」「主题」「设置」图标按钮。
- **中心**：多列瓦片画板，由 `WorkspaceLayout` 驱动。`WorkspaceTab` 当前项用浮起滑块；键盘焦点用焦点环，不与选中混用。
- **右栏**：顶带（高 `topBarHeight`）加底发丝线，左端放纯图标分段切换 `SegmentedControl`（`size: "md"`、`tone: "subtle"`、`iconCellWidth: Theme.sizeMd + Theme.spaceSm`：`folder`/`git-branch`/`history` 对应 `files`/`diff`/`history`，登记为标题带交互项，Windows 右端为 `WindowControls` 留空，空白区可拖窗），不再切出最右竖向页面栏；下方页体占满整栏宽度（`FileTreePane` 工具栏左侧带「文件」标题、下方加一条淡分隔线）。绑定聚焦终端的项目，从未聚焦则空。宽、折叠、当前页写入布局 JSON，未知页回落 `files`。聚焦远程终端（`boundRemoteHostId` 非空）或 Git 不可用时顶带只显示「文件」，只渲染对应文件页（见 [环境页](../../services/application/settings-appearance.md#环境页)）。页体四周 `spaceMd`，页内控件与行高亮共用"栏距再内缩 `spaceSm`"的左右边线。右栏不是瓦片区。
- **窄窗口**：窗口变窄时停靠的侧栏先收窄到最小宽度；仍放不下各列最小宽度时，侧栏改为浮动（先右后左，规则见 [瓦片布局 §5](../../services/workspace/layout-tiles.md#5-浮动侧栏)），不再占位。标题栏开关与 `app.toggleSidebar`/`app.toggleSidePanel` 都调 `WorkspaceController.toggleSidePanel(zone)`：浮动时从画布边缘滑出浮层（同一个侧栏实例换层级，不重建，状态保留），宽度沿用存储宽度、不超过画布 85%，带 `floating` 阴影，`reduceMotion` 时不播放滑动动画。浮层下铺透明捕获层：点击外部（这次点击被吞掉，不落到终端）或按 Esc 关闭浮层，焦点还给打开前的项；标题栏在捕获层之上，再点开关即关闭。浮层的接缝不显示、不可拖。窗口恢复宽度后按原来的开合意图重新停靠。

布局 JSON 含未知瓦片 kind 或结构不合法时整份拒绝，保持默认布局。

### 画板拆分

`WorkspaceCanvas.qml` 只负责分层（左栏槽、中心岛、右栏槽、接缝、外框线）；逻辑在同目录：

| 文件 | 职责 |
|---|---|
| `CanvasGeometry.qml` | 纯几何：列位置与宽（末列吃满）、行比例、落点命中与预览矩形 |
| `CanvasSeamController.qml` | 侧栏与列/行接缝拖拽：内存预览 + accent 预览线，松手只写一次布局；按住期间协调器进入本地 resize |
| `TileDragController.qml` | 瓦片拖放与 `dropRect` 预览（侧栏上方不接受落点） |
| `ZoneTileDelegate.qml` | 两个区域共用的瓦片委托 |
| `CanvasSeamHandle.qml` | 接缝把手，沿用 `ResizeHandle` 状态 |
| `CanvasDropTarget.qml` / `CanvasDragGhost.qml` | 落点高亮与拖拽幽灵 |

## 2. 瓦片类型（`isLegalKind`）

| kind | 组件 | 功能 |
|---|---|---|
| `projects` | `Sidebar.qml` | 项目、远程主机与会话（仅左栏）；列出的工作树以直角图标缩进挂在主项目下、主项目自己的会话之前，行右侧小字显示当前分支，文件夹缺失的项目变灰（见 [项目与会话](../../services/application/projects-sessions.md#工作树子项)）。工作树对话框（`WorktreeRootDialog`/`WorktreeDialog`）与强制删除确认挂在 `Main.qml`：侧栏是瓦片，可能不存在或不止一个 |
| `editor` | `PreviewPane.qml` | 只读查看 |
| `terminal` | `TerminalPane.qml` | 终端会话 |

### 终端头部

`ContentContainer.qml` 的 `terminalHeader`：终端图标、标题（占满剩余宽度），右侧依次是分支组、资源读数组与窗格关闭按钮，组间用竖向 `PixelLine`（高 `Theme.iconSm`）分隔，控件统一 `ChromeButton` `size: "sm"`、图标 `Theme.iconSm`、间距 `Theme.spaceSm`。

- **分支组**：分支按钮（图标与名称同属一个点击目标，浮窗打开时 `selected`；按钮接走了头部的按下事件，所以点击时自己调用 `activateThis()` 聚焦瓦片，和按在头部空白处一样）与有改动时的 `DiffStat`（见 [Git 读取 §4](../../services/git/reads-history-diff.md#4-引用与工作树)）。
- **资源读数组**：`ResourceStrip`，随瓦片聚焦滑入滑出（见 [资源监控 §2](../../services/monitoring/resource-monitoring.md#2-标题栏读数resourcestripqml)）。
- **弹层互斥**：分支浮窗与资源详情从不同时打开——两者都向下展开、相邻锚定，同时开着会互相叠压（分支浮窗的筛选框被挡）。打开分支浮窗前调用 `resourceStrip.dismiss()`；资源读数发出 `detailOpening()` 时关闭分支浮窗。固定的资源详情改用 `Theme.zBelow`，之后打开的弹层与菜单压在它上面。
- **窄宽度**：标题保底 `min(标题自然宽, Theme.dialogSm / 4)`；放不下时依次让出：资源读数（收起）→ 改动统计 → 分支名（`ChromeButton.maxWidth` 末尾省略，余量不足 `minLabelWidth` 时只留图标）→ 分支按钮。预算只读各控件的自然宽度（`naturalWidth`、`implicitWidth`），不读分配后的宽度，不形成绑定环；资源读数按它最近一次显示时的宽度（`ResourceStrip.fitWidth`，从未显示过时取实时宽度）计入——收起后它停止采样、数字复位，实时宽度会变窄，若按实时宽度判断，刚收起就又放得下，每次 polish 都在收起与展开间翻转（界面卡死）；这一宽度在下一轮事件循环（`Qt.callLater`）记下，因为 `shown` 经 `collapsed` 依赖它，在 `shown`/`perf` 的变更通知里同步写入会重入该绑定（绑定环告警）；分支名被截断时悬停提示显示全名。

## 3. 栅格

- 任意列数；每列一个全高瓦片（`rowSpan = 2`）或上下两个半高瓦片。
- 列宽比例 `columnWidths`，每列独立的行比例 `rowRatios`（默认 0.5）。竖接缝调相邻两列，横接缝只调本列。
- 每列有最小宽度（终端 440、编辑器 320、其余 250）；窗口放不下各列最小宽度之和时，各列按最小宽度的比例分摊中心区现有宽度（`tile_grid::distributeWidths`），不越出画板、不被右栏遮住，也没有横向滚动。此时列宽低于最小值，照常写入布局 JSON（恢复时宽度只要求为正，`fillCenterColumns` 按当时窗口重新分配）；相邻两列都已低于最小值时，列间拖动不改变宽度。

## 4. 拖放（`DropSlot`）

`WorkspaceLayout::dropSlotAt(xRatio, yRatio)`：`xRatio < 0.18` → `DropLeft`（左侧新列）；`> 0.82` → `DropRight`；否则按 `yRatio < 0.5` 取 `DropUpper`/`DropLower`（本列堆叠）。拖动中用 `previewCenterInsert` 预览，松手才提交。

新列的宽度（`tile_grid::openColumn`）：落在某列左右两侧时，新列与该列平分该列原宽，其余列宽度不变；整列满栈被挤出的瓦片同样平分目标列；落在最后一列之后（没有相邻列）或新建瓦片开出新列时取现有列的平均宽度。源列因拖走而消失时，它的宽度按比例分给剩余各列。

## 5. 关闭瓦片

- 终端：`ContentContainer` 弹出 `TerminalCloseDialog`，复选框默认取 `TerminalSettings.terminateOnTileClose`。勾选则移除瓦片、删除会话并终止进程树；否则只移除瓦片，会话后台运行。
- 查看器：直接关闭，无保存确认。
