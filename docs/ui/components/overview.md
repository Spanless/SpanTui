# 共享 UI 组件库

`qml/components/` 的分层规则、组件一览与调用约束。属性、信号以 QML 源码为准。

## 1. 分层

依赖方向只有 `business → common → base`：

- `base/`：弹层机制原语，只被 `AppPopup` 装配使用，business 不得实例化。
- `common/`：通用组件，business 的最深落点；不读领域字段（`gitRepository`、`SessionModel` 等），UI 偏好只经单例 `Appearance` 读取，不读 `TerminalSettings`/`WorkspaceSettings`（例外：`WindowControls` 读 `WindowChrome`，`Icon` 读 `UiIcons`）。
- `business/`：绑定领域服务，按域分目录。

`Main.qml`、`Settings.qml`、`TerminalPanel.qml`、`BoundaryGallery.qml` 是组合根；`Theme.qml`、`PixelGrid.qml`、`ScrollPolicy.qml` 是单例。组件在 `CMakeLists.txt` 的 `qml_components_*` 目标登记。

`Appearance` 的字号函数（`uiFont`/`uiAuxFont`/`formHintFont`/`editorFont`）最后一个参数必须传 `Appearance.fontRevision`——QML 看不到函数内部读了什么，漏传则字体变化时绑定不重算。

## 2. 选用规则

已有组件能承载时直接复用，不手写 `Rectangle` 表面、滚动条或弹窗逻辑；第二个相似用法出现时抽到 common。

| 需求 | 组件 |
|---|---|
| 文本 | `UiText`（界面）/ `MonoText`（等宽）/ `TerminalText`（终端字体）/ `SectionLabel`（分节标题）/ `FadeText`（只读限长，尾部淡出） |
| 输入 | `UiField`；列表过滤 `FilterBar`；查找 `FindBar`；数字 `Stepper` |
| 选择 | `AppComboBox`（`FontPicker`、`FontWeightPicker` 基于它）、`SegmentedControl`、`AppSwitch` |
| 按钮 | `ChromeButton`（`kind`：`secondary`/`primary`/`danger`/`ghost`/`selected`/`close`）；限宽用 `maxWidth`（标签末尾省略，余量不足 `minLabelWidth` 时带图标的按钮只留图标），按自然宽度做预算读 `naturalWidth`；开关语义用 `AppSwitch` |
| 弹层 | 模态 `AppDialog` / `ConfirmDialog` / `NameDialog`；锚定列表 `AnchorPopup`；裸锚定面板 `AnchorPanel`；菜单 `AppContextMenu`（行项 `ActionList`；带 `submenu` 的行悬停展开二级菜单，同时带 `action` 的为拆分行：点击执行默认项，悬停展开其余选项）；提示 `Tip` / `HelpTooltip` |
| 表面 | `CrispSurface`（圆角、描边、投影）；浮层 `PopupSurface`；分隔线 `PixelLine`；状态条 `InlineBanner` |
| 行与树 | `ListItem`（行原子）/ `NavRow`（图标+标签）/ `TreeListView` / `TreeGroupHeader` / `TreeChevron` / `TreeExpandHost`；行间滑动悬停 `GlideHighlight` |
| 滚动 | `DesktopScrollView` / `DesktopListView` / `DesktopFlickable`，都内建 `SlimScrollBar` 与 `DesktopWheelHandler` |
| 布局 | `GridRow` / `GridCol`（24 分栏）；设置项 `SettingRow`、`FontSettingsGroup` |
| 状态 | `EmptyState`、`PaneLoading`、`LoadingSpinner`、`OverflowProgressBar`、`StatusIcon`（成功/失败/警告/信息统一用它）、`DiffStat` |
| 其他 | `PaneHeader`、`ResizeHandle`、`WindowControls`（仅 `Main` 标题栏）、`Icon` / `FileIcon` / `ProgramIcon` |

`RenderText`/`RenderTextEdit` 是文本组件的渲染基座，业务不直接用。

## 3. 约束

**弹层**
- 打开与关闭的业务逻辑用 `opening(source)`/`popupClosed()`。`opening` 在定尺寸与定位之前发出：处理器里重置的内容（清筛选、复位只增宽度）决定弹层按什么尺寸放置。子类另写的 `onAboutToHide`/`onClosed` 与基类处理器叠加执行，不覆盖。
- 跟随锚点（`reposition()` 与逐帧跟随）以 `visible` 门控，不用 `opened`：`opened` 要等进场过渡结束才为真，而内容的隐式尺寸在第一帧 polish 时才定，按 `opened` 门控会在过渡结束后才校正位置，表现为弹层先偏一段再跳回。
- 外点关闭只由 `PopupLayerStack` 栈顶携带，一次外点只关最上层。
- 叠放顺序取决于弹层首次挂到 overlay 的先后与 `z`，不是打开先后；需要常驻在下层的弹层（固定的资源详情）设 `z: Theme.zBelow`。
- `PopupToggleGuard` 的解除用 `Timer`，不改回递归 `Qt.callLater`：守卫随弹层销毁后闭包会在失效上下文求值。
- `Overlay.modal/modeless` 遮罩收不到指针事件，不在遮罩里放 handler。

**按钮**
- 自定义内容（如 `ResourceStrip` 的读数）：不设 `text`/`iconName`，内容作为子项居中、颜色取 `fg`，`implicitWidth` 覆写为内容宽 + `labelPad * 2`；悬停、按下、缩放与提示仍归按钮。

**下拉框**
- 自定义行项写成 `ComboOptionRow { combo: root; ... }`；宽度取 `combo.listWidth`（不用 `ListView.view`），点击调 `pickIndex(index)`，不直接 emit `activated`（过滤后下标会错），声明 `glideRow: true`。
- `ComboPopupList` 高度取 `implicitHeight`，不给固定高度。
- 输入框在 `onOpened` 获焦；更早的 `forceActiveFocus` 会被 `AppPopup` 的 `focus: true` 覆盖。

**滚动**
- 滚动视口归父容器；子组件不自带滚动条，不自算 `contentWidth`。
- 不把 `DesktopListView` 套进 `DesktopScrollView`（两层滚动条与滚轮）。
- 视口在滚动条一侧延伸到容器视觉边缘，留白交给内容（`padding`、行宽或负边距）。
- 列表行按 `ListItem` 标准留出右侧空白；行内不可压缩部分（图标、`DiffStat`）设 `Layout.minimumWidth: implicitWidth`。
- 非 Flickable 视口才直接实例化 `SlimScrollBar`，目前只有 `TerminalSurface`。

**文本与表面**
- 不裸写 `Text {}`；`Text.font` 不暴露 `families`，回落链来自 C++ 的 `QFont`；不臆造 font 键（创建会中止）。
- `FadeText`/`UiField.fadeOverflow` 是叠底色渐变，`fadeColor` 必须等于所在表面色。
- 无圆角、无描边、无投影的大面积底板用 `Rectangle`（走不透明通道）；其余表面走 `CrispSurface`，不叠第二层轮廓。
- `Icon` 不包 `layer.smooth`/`mipmap`（图标发虚）。
- 相邻面板接缝只由一方画 `PixelLine`。

**布局与表单**
- `GridRow`/`GridCol` 之间不建高度绑定（绑定环）；不重命名为 `Row`/`Col`（会遮蔽 `QtQuick` 定位器）。
- `SettingRow` 行距只靠自身底部内边距，父容器与行都不加 spacing/外边距；`hint` 宽度不回读 `contentWidth`（绑定环）；警示走 `hint`+`hintTone`，不在控件槽并列徽标。
- 受控组件（`AppSwitch.checked`、`Stepper.value`、`FontSettingsGroup` 各值）由调用方持有，组件只发请求信号；`Stepper` 值井不挂 `Binding`（会吞掉未提交的键入）。

**列表行**
- 可展开行用 `ListItem.expandable`（按下时切换）；自写 `onClicked` 切换会在快速双击时丢第二次。
- `TreeListView` 的删除过渡保持零时长：存活的待删 delegate 会让重新展开排队 1–2s。
- 一个列表只放一个共享 `Tip`，由行发布锚点；逐行各持 `Tip` 在 `reuseItems` 下常驻等量弹层。

**窗口**
- `WindowControls` 三键保持 `HTCLIENT`，不上报 `HTMAXBUTTON`（Win11 Snap Layouts 浮层会抢最大化键）。

## 4. 改动完成标准

- [ ] 新增/删除组件：同步 `CMakeLists.txt` 登记与本文 §2；删除时全库清除引用及 [术语表](../../reference/glossary.md) 条目。
- [ ] 修改组件 API 或行为：全库搜索调用方；改动生命周期或关闭策略时确认 `AppPopup`/`PopupLayerStack` 协议不受影响。
- [ ] common/base 未引入领域字段或 `TerminalSettings`/`WorkspaceSettings`；business 未直接实例化 base。
- [ ] `tools/check-style.ps1` 通过。
