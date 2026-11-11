# 图标

## 1. 矢量图标（`UiIconCatalog`）

`assets/icons/<分类>/*.svg` 由 CMake 打包进 `ui_icons.qrc`，`UiIconCatalog`（`src/application/ui_icon_catalog.*`）启动时提取路径数据。图标为自绘，不复制第三方图标或品牌 Logo。

- 改图标改生成器：`tools/icons/` 的 `icons.py`（几何）、`motion.py`（动效），`python tools/icons/generate.py` 重写全部 SVG 与 `*.motion.json`（`--preview .dev/icon-motion.html` 出预览页）。不手改产物。
- 24×24 `viewBox`，图形居中于 3–21 安全框；1.5 线宽，圆端点、圆转角。
- 同类图标共用基准形：圆 r=8.5；横向框 18×14 r=3；纵向文档 14×18 r=3 折角 4.5；文件夹 18×15 r=3；短边 < 12 的框 r=2.5。
- 按语义命名与选用，一个图标只承载一种语义。文件类型（`types/`）：编程语言用单线字母组合，数据与媒体用象形。点只在承担语义时出现。
- 默认 `fill="none" stroke="currentColor"`；实心变体 `fill="currentColor" stroke="none"`。
- 解析器只读 `<path d>`，`<circle>`/`<rect>`/`<line>` 会被静默丢弃；多个 `<path>` 按序拼接，`<defs>` 内的不进静态字形。
- QML 只经 `Icon.qml` 渲染，不用 `Image` 加载 SVG。

## 2. 图标动效（`IconMotion`）

部分入口图标有逐个设计的动作；文件类型图标与窗口按钮没有。

- 宿主经 `Icon` 的 `hovered`/`pressed`/`active` 传入状态：
  - 悬浮：播放后保持末值，可循环；离开时缓出回静止。
  - 按下：播放一次，必须回到静止。
  - 激活：过渡到静止的激活姿态（语义色 + 一个部件变实心），不循环；激活期间屏蔽悬浮层的同名属性。
- 宿主映射：`ChromeButton`、`NavRow`、`SegmentedControl`、`ActionList`；其余用处不传状态。
- 图标落在同色实心底（accent/danger 高亮、`primary`/`close` 按钮）上时设 `tinted: false`，否则 `tint` 部件与底色同色而消失。
- 资源：SVG 中每个可动部件是带 `id` 的 `<path>`，拼接后等于静态字形；只在动画中出现的几何放进 `<defs>`。`<name>.motion.json` 的 `parts` 声明部件（`path`、`origin`、`parent` 须在自身之前、`solid`、`tint`、`rest`、`wrap`、`length`），`hover`/`press`/`active` 三组轨道；属性 `rotate`、`tx`/`ty`、`scale`/`sx`/`sy`、`opacity`、`draw`（需 `length`）、`morph`（两端命令结构一致）、`tint`、`fill`；关键帧 `[t, 增量, 缓动]`，缓动 `l`/`i`/`o`/`io`/`back`，各层相加。
- 加载时校验未知部件/属性/缓动、父级顺序、`draw` 缺长度、`morph` 结构不一致、非悬浮层循环、按下不回静止；任一失败则整个目录加载失败。
- 运行时：`IconMotionPlayer`（`src/application/icon_motion.cpp`）确定性求值，`IconMotion` 只在有部件运动时运行时钟。时间按真实流逝计算，不取动画驱动时间（驱动每帧固定步进，高刷屏上会加速）。静止时只画一条静态路径。创建时已激活的图标直接落在姿态上。减弱动效下不播放悬浮与按下，激活瞬切。

### 状态徽标

`status-success`/`-error`/`-warning`/`-info` 是 r=11 实心圆盘与镂空符号合成的单路径（奇偶填充），透出底色，随宿主 `color` 着色。激活层：圆盘从 0.6 弹回，符号经 `morph` 从种子形长出；悬浮层各有一个小动作。界面上用 `StatusIcon` 呈现。

### 加载图标

`loading`：中心提示符 `›`，外绕 6 粒尘埃组成的彗星（挂在 `orbit` 组下）。动效全在悬浮层：`orbit` 1100ms 一圈顺时针循环（`wrap` 360），尘埃转强调色并逐粒错峰闪烁，提示符缓慢呼吸。界面上用 `LoadingSpinner` 呈现（转动期间按住 `hovered`）。

## 3. 文件类型映射（`FileIconTheme`）

`src/workspace/file_icon_theme.cpp` 读 `:/icons/file-icons.json`，匹配顺序：精确文件名（`CMakeLists.txt`、`.gitignore`）→ 复合扩展名由长到短（`spec.ts` → `ts`）→ `file`、`folder`/`folder-open`。

## 4. 品牌图标

- 唯一源 `assets/brand/spantui.svg`。`tools/brand_icon_generator.cpp` 在构建时生成 `spantui.ico`：16–128 为 32 位 DIB + AND 遮罩，256 为 PNG；经 `assets/brand/spantui.rc` 嵌入 exe。
- 光标辉光与底板投影依赖 Qt SVG 6.7+ 的滤镜；改图后检查 ICO 各帧仍渲染滤镜。
- Shell 扩展包的 `StoreLogo.png`（50）、`Square44x44Logo.png`、`Square150x150Logo.png` 不随构建生成：改 SVG 后从新 ICO 的 256 帧以 Lanczos 缩放导出，同轮提交。
- 程序自带图标（Shell 等）走 `ProgramIcon.qml` + `src/platform/windows/program_icon_provider.*`，不属于矢量图标库。

测试：`tests/workspace/file_icon_theme_test.cpp`、`tests/application/ui_icon_catalog_test.cpp`、`tests/application/icon_motion_test.cpp`。
