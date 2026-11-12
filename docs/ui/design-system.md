# 设计系统

具体数值以 `qml/Theme.qml` 为准；本文只写选用规则。`AppearanceSettings`（QML `Appearance`）持久化 `themeMode`（`system`/`light`/`dark`）与字体设置。

## 1. 颜色

`Theme` 分两层：palette 层承载当前终端配色（`uiBackground`/`uiPlate`/`uiAccent`、状态层 `uiHover`/`uiSelection`/`uiPressed`、16 色 ANSI）；role 层由其派生语义角色，组件只消费 role 层。切换配色时 `Theme.applyScheme(surfaceColors)` 一次重算并过渡。

`applyScheme` 内的派生必须读目标值局部量：palette 属性带 `ColorAnimation`，赋值后回读得到的是动画中的旧值。

| 角色 | token | 用途 |
|---|---|---|
| 表面 | `surfaceBase` / `surfaceRaised` / `surfaceInset` / `overlaySurface` | 窗口 / 面板 / 输入与轨道 / 浮层；全部不透明 |
| 文本 | `text` / `muted` | 经 `readable()` 对面板色保底 4.5:1 |
| 线 | `border` / `separator` | 接缝与 `PixelLine` |
| 强调 | `accent`、`accentTop`、`accentSoft`、`accentText`、`textSelection` | 实心填充、焦点环、右键目标描边、拖动接缝、指示图标、选区；标签文字不用 accent；accent 填充上的文字用 `accentText` |
| 状态 | `success` / `warning` / `danger` 及 `*Soft`/`*Text` | 由 ANSI 派生 |
| 徽标填充 | `statusSuccess` / `statusError` / `statusWarning` / `statusInfo` | 只给 `StatusIcon`；固定色，不随配色，不作文字色 |
| 终端 | `terminalBackdrop`、`terminalForeground`、`terminalCursor`、`terminalSelection`、`terminalAnsi`、`terminalSearchMatch`/`terminalSearchCurrent` | 默认单元格不画底色，透出平面 |
| Git | `gitAdded` 等；`diffTint(underlay, kind)` | diff 行底色由宿主经 `TextLinesView.underlayColor` 声明底面 |

### 交互状态

- 悬停、选中、按下由配色方案的三个半透明状态层定义；缺省由 `completeSchemeUi()` 以前景色补齐。
- 唯一配方 `Theme.stateFill(surface, state, tone)` 把状态层合成到元素实际所在底面，得到实色。`state`：`hover`/`pressed`/`selected`/`selectedHover`/`selectedPressed`（选中垫在悬停/按下之下）。`tone`：`neutral`/`accent`/`info`/`success`/`warning`/`danger`，保留强度换色相。
- 实心填充件的悬停/按下用 `Theme.solidFill(fill, state)`（具名 `accentHover`、`accentPressed`、`dangerPressed`），不在调用点写 `Qt.darker`/`Qt.lighter`。
- 禁用用 `opacityDisabled`；焦点用焦点环；静态染色面用 `*Soft`。
- 具名结果按底面选：plate 上 `hover`/`pressed`/`selection`/`selectionHover`/`selectionPressed`；下沉面 `pressedOnInset`；浮起面 `raisedFillHover`/`raisedFillPressed`；浮层 `hoverOnOverlay`；其他底面直接调 `stateFill`。不把一个状态的 token 借给另一个状态。

### 状态语法

新组件先归类，再按该行取色：

| 元素 | 静止 | 悬停 | 按下 | 选中/开启 | 焦点 |
|---|---|---|---|---|---|
| 列表行 | 透明，正文色标签，`muted` 图标 | `hover` 滑块，图标取内容色 | `pressed` | `selection`（及其悬停/按下），不让位 | 不获焦 |
| 平面按钮 | 透明，`muted` | `hover` + 正文色 | `pressed` | 页签：浮起滑块；开启按钮：`surfaceInset`；均 `emphasis` | 焦点环 |
| 浮起按钮、下拉面 | `raisedFillTop`→`raisedFill` | `raisedFillHover` | `raisedFillPressed` | — | 焦点环 |
| 实心填充 | 色相本身 | `solidFill(…,"hover")` | `solidFill(…,"pressed")` | — | 焦点环 |
| 下沉轨道内 | `surfaceInset`/`switchTrackOff`，`muted` | 字形加深（开关关态叠 `hover`） | `pressedOnInset` | 浮起滑块 + `emphasis` | 焦点环 |
| 浮层列表 | 透明 | 实心 accent 滑块 + `accentText`（危险项 `danger`） | `accentPressed`/`dangerPressed` | 当前值行首 `check` | 键盘高亮同悬停 |
| 输入 | `surfaceInset` | — | — | 选区 `textSelection` | `raisedFill` + 焦点环；`invalid`/`warning` 描边 |
| 接缝 | `separator` | `muted` 加粗 | `accent` | — | — |
| 右键目标 | — | — | — | 菜单打开期间 accent 细描边，不改选中 | — |

层次只靠表面差、单发丝线与高程配方；无 Mica/Acrylic 或模糊层。瓦片不做焦点变色。

## 2. 字体

- 界面字体由 `Appearance.uiFont(size, Appearance.fontRevision)` 给出；编辑器与终端默认 Span Mono，界面默认 Span Sans（见 [内置字体](../services/application/settings-appearance.md#内置字体)）。
- 字体目录排除 DirectWrite 无法实例化的遗留 LOGFONT 族（`Roman`、`Swiss`、`Modern`、`Script`、`Decorative`、`Terminal`）与仅作图标回退的 Span Symbols。

## 3. 动效与透明度

- 状态颜色瞬时切换；按下反馈只动几何（`ChromeButton` 缩到 `Theme.pressScale`）。
- 时长只取 `durFast`（淡入淡出）/ `durMid`（位移、对话框）/ `durSlow`（强调进入）；`durTheme` 专用于换色。装饰性循环写成命名属性。
- "抵达"类动画用 `Theme.easeEmphasized`（`Easing.BezierSpline`）；淡入淡出用 `OutCubic`/`InCubic`；成组入场按 `durStagger` 错峰并设上限。
- `opacity` 只取 `fadeSm`/`fadeMd`/`fadeLg`，禁用用 `opacityDisabled`。填充一律实色；半透明只用于发丝线与高程轮廓、`scrim`、高程投影、焦点环。
- tooltip 时序只取 `tipDelay`/`tipDelayQuick`/`tipTimeout`。
- 非模态弹层共用 `PopupEnter`/`PopupExit`；模态对话框用 `AppDialog` 的 slide+pop。减弱动效见 [无障碍与动效](accessibility-motion.md)。

## 4. 形状与高程

- 圆角阶梯 `radiusXs`（徽章、色块）/ `radiusSm`（按钮、输入、行）/ `radiusMd`（容纳滑块的轨道、横幅）/ `radiusLg`（浮层）/ `radiusXl`（模态对话框）；窗格直角；胶囊写 `height / 2`。
- 同心：嵌套形状在内距 d 处取 `外半径 − d`；轨道与滑块间隙 `thumbInset`（= `radiusMd − radiusSm`）。
- `CrispSurface.elevation`：

  | 级别 | 填充 | 轮廓 | 投影 | 用途 |
  |---|---|---|---|---|
  | `sunken` | `surfaceInset` | 无 | 无 | 分段、步进轨道 |
  | `none` | 调用方 | 调用方 | 无 | 面板、行 |
  | `raised` | `raisedFillTop`→`raisedFill` | `raisedRim`；按钮与下拉面用更实的 `buttonRim`（背后无底槽衬托） | `shadowRaised` | 滑块、圆钮、页签、按钮、下拉面 |
  | `floating` | `overlaySurface` | `floatingRim` | `shadowFloating` | 菜单、下拉、tooltip、对话框、拖拽影 |

  投影 sigma 与偏移是逻辑像素，由 `CrispSurface` 乘 DPR；`focusRingBlurPx` 是物理像素。`focusRing: true` 接管描边与投影。调用点不写投影数值（lint `bare-shadow`）。
- 静止控件不画描边；accent 描边只随焦点环与右键目标出现。
- 窗格内容到边框的 `spaceMd` 留白由容器提供（`RightDock` body、侧栏列表），页面组件不另加。行内装饰（如 Git 徽章）放不下时整体隐藏。

## 5. 尺寸阶梯

4px 网格三档，常规用中档：

| 域 | token | 选档 |
|---|---|---|
| 间距 | `spaceSm`/`spaceMd`/`spaceLg` | 图标↔文字 / 行内边距与间隔 / 面板边距与分节 |
| 高度 | `sizeSm`/`sizeMd`/`sizeLg`（随界面字号缩放） | 紧凑按钮 / 列表行与默认控件 / 面板头与主操作 |
| 图标 | `iconSm`/`iconMd`/`iconLg` | 次级字形与紧凑按钮 / 主图标 / 空态 |
| 文字 | `uiSm`/`uiMd`/`uiHint` | 次信息（元数据、标签、提示、读数）/ 主信息（条目、标题、正文）/ 表单提示；渲染字号等于车道配置值，不加减派生 |
| 弹窗 | `dialogSm`/`dialogMd`/`dialogLg` | 命名确认 / 选择器 / 编辑器与宽预览 |
| 表单 | `fieldMd`、`formLabelW` | 字段默认宽 / `SettingRow` 标签列宽 |
| 滚动 | `scrollPad`、`scrollEdgeGap` | 视口内边距 / 拇指离外缘距离 |

`uiSm` 走辅助字体车道（`Appearance.uiAuxFont`），`uiHint` 走表单提示车道（`Appearance.formHintFont`）；`UiText size:"lg"` 与 md 同字号，仅加粗；等宽次级字形用 `MonoText px: uiSm`。

档位是下限，文字决定上限（各字体车道可独立调大）：

- 文字控件隐式高取 `Theme.fitText(档位, FontMetrics.height)`，清零 Controls 样式的默认上下 padding（否则小档位会裁字）。
- 含文字的行用 `Layout.minimumHeight: Theme.size*`，不用固定高度钉死；固定高度只给无文字铬件与统一列表行。
- 业务不直接声明 `TextField`/`TextInput`/`TextArea`。

列表行：`sizeMd` 高、`spaceMd` 内边距、`spaceSm` 图标间隔、`iconMd` 主图标；`ListItem` 在 delegate 内跟随视图的 `itemHeight`/`itemPadX`。

z 值：通用 `zBelow`/`zRaised`；主窗口根兄弟 `zWorkspace` → `zChrome` → `zToast` → `zModal`；组件内部层序写成命名属性。

## 6. 样式检查

`tools/check-style.ps1` 逐行扫描 `qml/`，拦截退役 token、`compact:` 布尔、裸数值（间距、圆角、尺寸、几何、字号、透明度、时长、z）、裸 tooltip 时序、裸 `Text {}`、调色板外十六进制色，以及：

- `raw-control`：business 与 `qml/*.qml` 不声明裸 `ToolTip {}`/`Popup {}`；
- `raw-text-input`：同范围不声明裸 `TextField`/`TextInput`/`TextArea`；
- `shared-preferences`：common/base 不出现 `TerminalSettings`/`WorkspaceSettings`，偏好只读 `Appearance`；
- `font-revision`：`Appearance` 字号函数必须带 `Appearance.fontRevision`；
- `bare-shadow`：投影通道不写非零字面量（`CrispSurface.qml` 除外）。

特例同行写 `// style-ok: 原因`；规则可用 `Scope` 正则限定范围。

允许的例外：内容专有尺寸（diff gutter、列宽、色板格、矢量画布）写成命名属性；窗口默认几何；`BoundaryGallery` 等夹具的探针几何。共享控件用 `size: "sm"|"md"` 选档。
