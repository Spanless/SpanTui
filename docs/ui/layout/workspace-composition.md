# 工作区组合

主窗口 = 标题栏（`WindowControls`）+ `WorkspaceCanvas`。设置不是瓦片：`window.page === "settings"` 时 `Main.qml` 的 `settingsPageLoader` 实例化 `Settings` 并 `open()`（`AppPopup` 模态，遮罩覆盖整窗）；关闭经 `popupClosed` 复位 `page === "workspace"`。

## 1. 区域

- **标题栏**：只有左右侧栏开关（`panel-left`/`panel-right`）；应用入口（消息、更新、主题、设置）固定在左栏底部。
- **左栏**：「本地」节（项目与会话；无项目时显示只占自身高度的空态，不自动弹文件夹选择器）与「远程」节（SSH 主机与会话，最多占栏高 40%，双击主机新开终端）。ssh 不可用时隐藏「远程」节。
- **中心**：多列瓦片画板，由 `WorkspaceLayout` 驱动。`WorkspaceTab` 当前项用浮起滑块；键盘焦点用焦点环，不与选中混用。
- **右栏**：页体 + 最右竖向页面栏（`files`/`diff`/`history` 三个图标按钮）。绑定聚焦终端的项目，从未聚焦则空。宽、折叠、当前页写入布局 JSON，未知页回落 `files`。聚焦远程终端（`boundRemoteHostId` 非空）时只显示该主机的 `RemoteFilePane` 并隐藏页面栏；Git 不可用时只有文件页（见 [环境页](../../services/application/settings-appearance.md#环境页)）。页体四周 `spaceMd`，页内控件与行高亮共用"栏距再内缩 `spaceSm`"的左右边线。右栏不是瓦片区。

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
| `projects` | `Sidebar.qml` | 项目、远程主机与会话（仅左栏） |
| `editor` | `PreviewPane.qml` | 只读查看 |
| `terminal` | `TerminalPane.qml` | 终端会话 |

### 终端头部

`ContentContainer.qml` 的 `terminalHeader`：终端图标、标题（占满剩余宽度），右侧依次是分支组、资源读数组与窗格关闭按钮，组间用竖向 `PixelLine`（高 `Theme.iconSm`）分隔，控件统一 `ChromeButton` `size: "sm"`、图标 `Theme.iconSm`、间距 `Theme.spaceSm`。

- **分支组**：分支按钮（图标与名称同属一个点击目标，浮窗打开时 `selected`；按钮接走了头部的按下事件，所以点击时自己调用 `activateThis()` 聚焦瓦片，和按在头部空白处一样）与有改动时的 `DiffStat`（见 [Git 读取 §4](../../services/git/reads-history-diff.md#4-引用与工作树)）。
- **资源读数组**：`ResourceStrip`，随瓦片聚焦滑入滑出（见 [资源监控 §2](../../services/monitoring/resource-monitoring.md#2-标题栏读数resourcestripqml)）。
- **弹层互斥**：分支浮窗与资源详情从不同时打开——两者都向下展开、相邻锚定，同时开着会互相叠压（分支浮窗的筛选框被挡）。打开分支浮窗前调用 `resourceStrip.dismiss()`；资源读数发出 `detailOpening()` 时关闭分支浮窗。固定的资源详情改用 `Theme.zBelow`，之后打开的弹层与菜单压在它上面。
- **窄宽度**：标题保底 `min(标题自然宽, Theme.dialogSm / 4)`；放不下时依次让出：资源读数（收起）→ 改动统计 → 分支名（`ChromeButton.maxWidth` 末尾省略，余量不足 `minLabelWidth` 时只留图标）→ 分支按钮。预算只读各控件的自然宽度（`naturalWidth`、`implicitWidth`），不读分配后的宽度，不形成绑定环；分支名被截断时悬停提示显示全名。

## 3. 栅格

- 任意列数；每列一个全高瓦片（`rowSpan = 2`）或上下两个半高瓦片。
- 列宽比例 `columnWidths`，每列独立的行比例 `rowRatios`（默认 0.5）。竖接缝调相邻两列，横接缝只调本列。
- 每列有最小宽度；窗口不够宽时超出部分被裁剪（无横向滚动）。

## 4. 拖放（`DropSlot`）

`WorkspaceLayout::dropSlotAt(xRatio, yRatio)`：`xRatio < 0.18` → `DropLeft`（左侧新列）；`> 0.82` → `DropRight`；否则按 `yRatio < 0.5` 取 `DropUpper`/`DropLower`（本列堆叠）。拖动中用 `previewCenterInsert` 预览，松手才提交。

## 5. 关闭瓦片

- 终端：`ContentContainer` 弹出 `TerminalCloseDialog`，复选框默认取 `TerminalSettings.terminateOnTileClose`。勾选则移除瓦片、删除会话并终止进程树；否则只移除瓦片，会话后台运行。
- 查看器：直接关闭，无保存确认。
