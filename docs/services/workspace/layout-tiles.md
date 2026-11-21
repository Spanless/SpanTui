# 瓦片布局模型

瓦片类型、栅格与落点区域见 [工作区组合](../../ui/layout/workspace-composition.md)。实现分三个文件：

| 文件 | 职责 |
|---|---|
| `src/workspace/workspace_layout.*` | `WorkspaceLayout`（`QAbstractListModel`）：持有瓦片、画板几何与视口事务，发信号 |
| `src/workspace/tile_grid.*` | 网格计算的纯函数：列占用、最小宽度、宽度分配、移动瓦片、落点；不持状态，不依赖 `QObject` |
| `src/workspace/workspace_layout_json.*` | 持久化格式：`LayoutSnapshot` 与 v4 JSON 互转，含结构校验 |

## 1. 宽度分配

`tile_grid::distributeWidths` 按 `columnWidths` 权重分配中心区宽度，每列不低于其瓦片最小宽度；放不下最小宽度之和时，各列按最小宽度的比例分摊现有宽度（余数给最后一列），不越出中心区。用例：`workspace_layout_test` 的 `narrowWindowSharesCenterWidthByMinimumRatio`、`squeezedLayoutRestoresFromJson`。

## 2. 拖放

- `dropSlotAt(xRatio, yRatio)` 给出 `DropLeft`/`DropRight`/`DropUpper`/`DropLower`。
- 拖动中 `previewCenterInsert()` 只生成内存预览，松手才由 `insertCenterTile()` 提交。
- **自落点**：插入结果与当前放置相同时（如独占一列的全高瓦片落回本列），`tile_grid::moveTile` 返回 `false`：预览 `ok=false`，提交不改布局、不重置列宽、不发 `tilesChanged`。堆叠列中把上方瓦片拖到本列下半区仍按交换处理。用例：`workspace_layout_test` 的 `dropOnOwn*`。

## 3. 引用对账

`reconcileReferences(scopes, sessionScopes)`（有效作用域键列表、会话 id 到作用域键的映射）处理绑定项目的瓦片（`tile_grid::isProjectBoundKind`：`terminal`、`editor`）：移除已关闭项目的查看器、指向已删除会话的终端，并合并相邻半高瓦片。远程作用域视为有效。

## 4. 视口与持久化

- `previewViewport(width, height)` 只更新内存坐标并发 `geometryChanged`；`commitViewport()` 发 `layoutCommitted`（事务与代次见 [视口事务](../../ui/layout/viewport-transitions.md)）。
- 只在 `layoutCommitted` 与 `tilesChanged` 时写 v4 JSON（设置键 `workspace.layoutJson`）。
- 瓦片只写 `id`、`kind`、`row`、`rowSpan`、`column`、`sessionId`、`projectPath`；区域由 kind 推出（`projects` 在左栏），读取时忽略 `zone`/`colSpan`。缺 `tiles`、`columnWidths`、`rowRatios` 任一即整份拒绝；列宽只要求为正（窄窗口下保存的列宽可以低于最小宽度，恢复后按窗口重新分配）；未知 kind 同样拒绝。`layoutFromJson` 只做结构校验，侧栏宽度、右栏页与行比例按存储值返回，由 `WorkspaceLayout::restoreFromJson` 钳制。
- 右栏默认宽 `kDefaultRightWidth`（300px），JSON 缺 `rightWidth` 时同样回落；`kMinRightWidth`（240px）为下限。

## 5. 浮动侧栏

窗口太窄时侧栏不再占位，宽度让给中心区；交互闭环见 [工作区组合 §1](../../ui/layout/workspace-composition.md#1-区域)。

- **先收窄，再浮动**：中心区需要的宽度 = 各列瓦片最小宽度之和（不低于 `kMinCenterWidth`）。窗口变窄时，停靠的侧栏先让出宽度（右侧先，再左侧），各自最多收窄到 `kMinRightWidth`/`kMinLeftWidth`；`leftWidth`/`rightWidth` 返回这个实际宽度，存储宽度（`leftPanelWidth`/`rightPanelWidth`，持久化）不变，窗口变宽后自动回到存储宽度。
- **派生、不持久化**：`leftFloating`/`rightFloating` 每次重算列宽时（`fillCenterColumns`，含视口预览与提交、侧栏宽度变化、插入与对账）由 `planSides` 推出，判定按侧栏的**最小**宽度计：侧栏都收到最小仍让中心区低于需要宽度时，先浮右侧，仍不够再浮左侧；恢复停靠要多出 `kSideDockHysteresis`（48px）才生效，先左后右，临界宽度上不来回跳。只有展开的一侧（`leftCollapsed`/`rightCollapsed` 为假，左侧还须有项目瓦片）参与判定。
- **宽度**：浮动的一侧在 `leftWidth`/`rightWidth`、`islandX` 与中心区宽度里按 0 计；浮层用存储宽度。两侧都浮动仍放不下时，§1 的按最小宽度比例分摊兜底。
- **意图与浮层**：持久化的 `leftCollapsed`/`rightCollapsed` 表示"宽窗口下的停靠意图"，含义不变。`setSideShown(zone, shown)`：停靠时改折叠意图；浮动时只开关瞬时的 `leftOverlayOpen`/`rightOverlayOpen`（打开一侧会关闭另一侧），不写 JSON。窄窗口里打开一个已折叠的侧栏会先把意图改为展开，它随即浮动并以浮层打开。`closeOverlays()` 关闭两侧浮层；一侧恢复停靠时它的浮层随之关闭。`leftShown`/`rightShown` 是开关按钮的高亮：浮动看浮层，停靠看意图。以上状态变化合并为一次 `sidesChanged`。
- **钳制**：`clampedSideWidth(zone, px, windowInnerWidth)` 把拖拽中的侧栏宽度限制在 `[kSideCollapseWidth, 窗宽 − 另一侧 − 中心最小宽度]`，`CanvasSeamController` 用它代替 QML 字面量。

测试：`workspace_layout_test`（`narrowingFloatsRightSideThenLeft`、`wideningDocksSidesBackPastHysteresis`、`floatingSideTogglesOverlayNotIntent`、`showingSideInNarrowWindowSlidesItIn`）、`workspace_controller_test`。
