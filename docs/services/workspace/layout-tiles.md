# 瓦片布局模型

瓦片类型、栅格与落点区域见 [工作区组合](../../ui/layout/workspace-composition.md)。实现分三个文件：

| 文件 | 职责 |
|---|---|
| `src/workspace/workspace_layout.*` | `WorkspaceLayout`（`QAbstractListModel`）：持有瓦片、画板几何与视口事务，发信号 |
| `src/workspace/tile_grid.*` | 网格计算的纯函数：列占用、最小宽度、宽度分配、移动瓦片、落点；不持状态，不依赖 `QObject` |
| `src/workspace/workspace_layout_json.*` | 持久化格式：`LayoutSnapshot` 与 v4 JSON 互转，含结构校验 |

## 1. 宽度分配

`tile_grid::distributeWidths` 按 `columnWidths` 权重分配中心区宽度，每列不低于其瓦片最小宽度；不够时各列取最小宽度，超出部分被裁剪。

## 2. 拖放

- `dropSlotAt(xRatio, yRatio)` 给出 `DropLeft`/`DropRight`/`DropUpper`/`DropLower`。
- 拖动中 `previewCenterInsert()` 只生成内存预览，松手才由 `insertCenterTile()` 提交。
- **自落点**：插入结果与当前放置相同时（如独占一列的全高瓦片落回本列），`tile_grid::moveTile` 返回 `false`：预览 `ok=false`，提交不改布局、不重置列宽、不发 `tilesChanged`。堆叠列中把上方瓦片拖到本列下半区仍按交换处理。用例：`workspace_layout_test` 的 `dropOnOwn*`。

## 3. 引用对账

`reconcileReferences(scopes, sessionScopes)`（有效作用域键列表、会话 id 到作用域键的映射）处理绑定项目的瓦片（`tile_grid::isProjectBoundKind`：`terminal`、`editor`）：移除已关闭项目的查看器、指向已删除会话的终端，并合并相邻半高瓦片。远程作用域视为有效。

## 4. 视口与持久化

- `previewViewport(width, height)` 只更新内存坐标并发 `geometryChanged`；`commitViewport()` 发 `layoutCommitted`（事务与代次见 [视口事务](../../ui/layout/viewport-transitions.md)）。
- 只在 `layoutCommitted` 与 `tilesChanged` 时写 v4 JSON（设置键 `workspace.layoutJson`）。
- 瓦片只写 `id`、`kind`、`row`、`rowSpan`、`column`、`sessionId`、`projectPath`；区域由 kind 推出（`projects` 在左栏），读取时忽略 `zone`/`colSpan`。缺 `tiles`、`columnWidths`、`rowRatios` 任一即整份拒绝；未知 kind 同样拒绝。`layoutFromJson` 只做结构校验，侧栏宽度、右栏页与行比例按存储值返回，由 `WorkspaceLayout::restoreFromJson` 钳制。
- 右栏默认宽 `kDefaultRightWidth`（300px），JSON 缺 `rightWidth` 时同样回落；`kMinRightWidth`（240px）为下限。

测试：`workspace_layout_test`、`workspace_controller_test`。
