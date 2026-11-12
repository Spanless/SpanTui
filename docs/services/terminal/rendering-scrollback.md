# 终端渲染与滚动历史

## 1. 场景图渲染与快照交换

```text
TerminalCoreWorker → TerminalFrameStore → TerminalFramePreparer → TerminalView::updatePolish → TerminalSceneGraph::sync → GPU
                     （锁内换指针）         （每 View 一个线程）     （GUI 接受帧）              （同步阶段）
```

### 发布调度（`TerminalPresentationScheduler`）

Core 何时发布快照只由 `src/terminal/core/terminal_presentation_scheduler.*` 决定。Core 上报事件，按返回的复查时间启动唯一的 `QTimer`；时间戳由调用方传入，规则可用虚拟时钟测试。

- **切点**：只有两处解析器状态是完整画面：生产者一次写入的结尾（`OutputCallback` 的 `endsWrite`，经 `TerminalOutputQueue` 保存）与 DEC 2026 结束。输出触发的发布只在切点进行，Core 每消费一块输出或遇到 2026 结束都评估一次。
- **写入中途**：一次写入迟迟不结束时，从首个未发布变化起最多等 `kMidWriteHoldMs`（50ms）后强制切开，计入视口追踪的 `forcedCut`/`forcedCuts`。
- **帧率**：切点按所选帧率发布；后台会话（未挂载渲染）至少间隔 100ms。网格变化、消费者挂载、传输结束、视口滚动、选区与查找跳转立即发布，但不进入 2026 事务或隐式重绘。
- **2026 事务**：事务内不发布；字节停止 `kSynchronizedStallMs`（250ms）后强制释放并告警。
- **resize 后合并与 ED3 隐式重绘**：见 §5.3。

### 快照（`TerminalFrameSnapshot`）

`frame.lines` 是视口及上下 overscan 行（`viewportRow` 为视口首行下标），每行带 128 位 `TerminalRowId`（行移动不变，永不复用）与修订号。另含滚动条（`total`/`offset`/`rows`，以最旧历史行为 0）、光标、每行选区与查找列区间、Kitty 放置、查找状态、视口 epoch、transport epoch、`scrollRequest`（已反映的最后一次滚动请求）、`viewportJump`（Core 自己移动视口时递增）、`fullRefresh`。

### 滚动（`TerminalScrollModel` + Core 视口）

视口位置归 libghostty：离开底部后钉在原内容上，跟随底部时随输出前进。备用屏固定在底部。

- View 以"距最旧历史行的行数"（可带小数）表示目标与动画位置。滚轮刻度按 τ=40ms 指数缓动，由 `QQuickWindow::afterAnimating` 逐帧推进，每帧以 `QQuickWindow::update()` 请求下一帧（条目自身的 `update()` 若发生在同步期间会并入本帧，动画停住）；单帧最多推进 `kMaxFrameMs`（33ms）。触控板像素增量、拖动滚动条与减弱动效（`smoothScrolling=false`）直接跳转。
- 整数行变化时经 `TerminalSession::scrollViewport`/`scrollViewportToBottom` 请求 Core（`ghostty_terminal_scroll_viewport` 后立即发布）；小数部分不发。
- overscan 平时上下各一屏。一次跳过超过一屏（拖动滚动条）时收窄到各一行，请求停止 `kOverscanSettleMs`（150ms）后放宽并补发一帧——拖动时几乎每行都是新行，宽 overscan 让 Core 与准备线程做三倍工作。
- 绘制按"位置 − 帧 `offset`"取可见行，按 `−小数行×行高` 平移并裁剪。目标超出 overscan 时画帧内离目标最近的一屏，停在边缘等新帧；不退回帧自身的视口位置，否则拖动时文字来回震动。
- 帧的 `viewportJump` 变化（查找、提示符跳转、选区自动滚动、输入回底、resize）时 View 采用帧位置；Core 已处理完最后一次请求而 View 静止且位置不同时（历史满后淘汰最旧行）也采用。
- 离开底部时历史增长或视口被推动即置未读，回到底部清除；打字把视口送回底部。

### 准备线程（`TerminalFramePreparer`）

全部 View 共用一个线程（进程退出时停止），每个 View 一个 `TerminalFramePreparer` 与自己的字形缓存，把快照加工成只读的 `TerminalPreparedFrame`：先为要重建的行生成 `TerminalRowLayout`，一次光栅化缺失字形进本视图图集，再由 `TerminalRowGeometryBuilder` 把每行写成一份顶点数组。场景同步不排版、不整形。

- **复用**：按（行 id，修订号）复用，与行在帧中的位置无关；还要求样式代次、基线、transport epoch、列数相同且行画在当前图集上；空行 id 不复用。
- **只做最新**：准备期间到达的多个快照只留最新一个；`prepared` 排队回 GUI。
- **析构**：只等本视图在途的一帧；工作对象随后在准备线程删除，尚未执行的排队准备变为空操作。
- **样式**：字体、DPR、前景/背景、调色板合成带代次的 `TerminalRenderStyle`。View 只呈现当前代次的帧；度量变化换新图集。已准备帧带自己的格子尺寸，场景同步按它绘制，避免新旧尺寸混用。
- **闪烁**：含闪烁字符的行另备一份闪烁格透明的顶点，与可见顶点共用索引，定时器只切换上传哪一份。

### 字形图集（`TerminalGlyphCache` / `TerminalGlyphAtlas`）

- 每种"文字 + 字体 + 格宽"只整形、光栅化一次（基线对齐设备像素，宽字符在两格内居中）。光栅化在 `TerminalGlyphRasterizer`，图集只管查找与放置。
- 两页固定大小方形纹理：遮罩页 R8（首张 2048²，上限 8192²），存文字距离场、特殊字形覆盖率与左上角不透明块；彩色页预乘 RGBA8（首张 512²，上限 2048²），只存彩色字形。
- 文字用 Qt 的 `QDistanceField`（规则与 Qt Quick 相同），一格内全部字形合成一条路径，四周留空纹素防子像素采样越界。彩色字体白色绘制，偏离白色的像素进彩色页，其余取 alpha 进遮罩页。框线、方块、盲文按 GPU 光栅规则在 CPU 上光栅化成覆盖率（§2），之后与文字一样每格一个四边形。
- `fontForStyle()` 每次新建 `QFont`，同一行同样式的格子共用一份；图集先按字体私有数据比较。
- 缺失字形多时（每 16 个一个帮手，至多 3 个且不超过核数减一）由缓存的小线程池并行光栅化，各用自己的光栅器，结果在准备线程依次放入。
- **替换**：图集不扩容、不淘汰。某页满时换新图集，满页尺寸翻倍（至上限），按最近使用把字形逐纹素复制过去直到占满各页一半。正在写的行写完后若图集已换则整行重写，保证一行只采样一张图集。行持有图集的 `shared_ptr`；画在旧图集上的行下次准备时不复用，避免旧图集被长期占住。
- 图集归各自的准备器，不跨视图共享。像素在准备线程写、渲染线程读，经图集互斥交接；上传只取新写区域及其右下透明间隔，变更日志过长时上传各页已用行。

### 字体隔离

`QFont` 的隐式共享数据缓存字体引擎并在读取时无锁改写，跨线程共享会破坏堆。准备线程收到新样式先 `TerminalFontSet::isolate()`；光栅器整形前取 `isolatedCopy`。`QRawFont` 只属于创建它的线程：帮手每批新建光栅器、用完即弃，字形缓存在准备线程的工作对象中并在该线程删除。

### 接受、ACK 与场景图

- 快照接受（`terminal_view.cpp` 的 `acceptSnapshot`）：非零视口期望必须匹配，过期快照只 ACK 不替换；滚动位置不参与。ACK 在 View 采用或判定等待已准备帧时发出，故每个终端同时最多一帧在加工。
- 已准备帧在下一个窗口帧换上，不排队、不限流。
- `TerminalSceneGraph::sync` 只比较行指针，相同即跳过；先按指针重排行节点，只为新内容复制顶点。每行一个变换节点 + 几何节点，材质 `TerminalCellMaterial`（`shaders/terminal_cell.*`）：遮罩页是顶点预乘色的覆盖率；彩色字形取彩色页 × 顶点色；距离场按 Qt Quick 规则转覆盖率（`terminal_cell_material` 的 `TerminalFieldText`，阈值由纹理坐标屏幕导数算出）。Direct3D 用子像素版本 `terminal_cell_subpixel.frag`（双源混合，一行任意颜色一次画完），其他 API 用灰度版本。材质带 `RequiresFullMatrix`，行不合批，滚动只改矩阵。顶点着色器把行原点移到最近设备像素（条目可能落在半像素上）。
- 每张图集在渲染线程对应两个 `TerminalAtlasTexture` 与一份材质，无行使用时删除。`software` 后端（离屏平台）画不了自定义材质，改由 `TerminalCellRasterizer` 按灰度着色器规则在 CPU 上画行图像，用纹理节点显示。IME 预编辑层用 `QSGTextNode`。
- **图形**：Kitty 图像的存储与淘汰在 libghostty。Core 按"图像 id + 代次 + 源矩形"转成共享的 `GraphicResource`，仍有放置时才保留；`graphicsRevision` 在放置或像素变化时递增。场景同步按 `graphicRect` 画在 `graphicsLayer`，格宽有小数时按 `格宽 / round(格宽)` 缩放。QSG 纹理只在同步阶段创建或删除。
- **析构**：`~QQuickItem` 在派生成员析构后仍会发 `windowChanged` 等信号，`TerminalView` 析构开头先断开自身与会话的连接。

## 2. 特殊字形几何

`src/terminal/render/terminal_drawn_glyphs.cpp` 自绘以下字符，避免字体外边距与舍入造成断线、缝隙：

| 组件 | 范围 |
|---|---|
| `TerminalBoxDrawing` | 制表符 U+2500..U+257F，对齐半像素设备坐标 |
| `TerminalBlockElements` | 块元素 U+2580..U+259F、六分格 U+1FB00..U+1FB3B；整行六分格合成一个矩形 |
| `TerminalBraille` | 盲文 U+2800..U+28FF |
| `TerminalStrokeMeshBuilder` | 笔画三角化为带覆盖率的 `TerminalCoverageMesh`，光栅化进图集一次 |

`TerminalFontSet`（`terminal_font_set.cpp`）回退链：主字体 → 用户回退 → Span Mono → Span Symbols（见 [内置字体](../application/settings-appearance.md#内置字体)）→ Segoe UI Emoji → Microsoft YaHei UI；主字体不可用时以 Span Mono 兜底。整形前经 `drawableText()` 把没有任何矢量字体收录的码位换成 `U+FFFD`（无处可画的组合附标丢弃，连接符、方向与变体选择符保留），否则 Qt 会试遍遗留位图族，每族每字号一条 DirectWrite 告警。判定结果进程内缓存。

## 3. 光标与内容帧率

- `TerminalCursorAnimation`：聚焦空闲时 1060ms 正弦呼吸（30 Hz）；减弱动效下 530ms 离散闪烁；内容帧率 ≥ 15 时暂停。
- 内容帧率：渲染线程在 `afterRendering` 为新内容帧递增原子计数，GUI 每 500ms 读取。只在视图可见且窗口未隐藏/最小化时采样。计数器由视图与处理函数共同持有（`shared_ptr`）：断开连接不等待另一线程上正在执行的调用，处理函数不得访问视图本身。

## 4. 滚动历史、选区与查找

屏幕、历史、视口、选区与查找结果都在 `GhosttyEngine` 的 libghostty 实例中，只在 Core 线程访问。

- **历史**：按页保存，改列宽时与屏幕一起 reflow。上限：行数（`maxScrollback`，默认 10 万）与字节（`kScrollbackMaxBytes`，128 MiB），按页裁剪，均为近似。输出停止 10s 后压缩空闲页（`ghostty_terminal_compress`：有消费者 `INCREMENTAL`，后台 `FULL`），不在输出路径上压缩。
- **清屏**：`clearDisplay` 清除历史与光标行以外的行，光标行移到顶部；RIS 清空全部历史。只清本地会与 conhost 的屏幕副本失配（之后的增量输出落在错位置），因此 Core 发 `displayCleared`，会话调进程 `clearScreen` → `ConptyClearPseudoConsole(hpc, keepCursorRow=TRUE)`。系统 ConPTY（`--conpty=system`）无此入口，只清本地。

### 4.1 选区与复制

- View 把按下、拖动、松开连同格子、像素、时间戳与双击间隔作为 `TerminalSelectionEvent` 发给 Core，由 libghostty 手势解析：拖动选字符、双击选词、三击选行、Alt 拖动矩形；拖出视口时 Core 每 50ms 滚一行并延伸选区（`viewportJump` 递增）。
- 帧带每行选区列区间（`Frame::selection`），在 `selectionLayer` 绘制，随行移动。
- `TerminalSession::selectedText` 以阻塞排队调用向 Core 取文本（软换行拼接、去行尾空白），View 写剪贴板。

### 4.2 查找

- 范围：全部历史与当前屏（备用屏时只查当前屏）；字面文本，仅 ASCII 字母不区分大小写。
- `TerminalView::setSearch` → `TerminalSession` → Core `setSearchNeedle`。Core 用零间隔定时器分步推进（每步至多 64 次 `tick`），步间让输出与输入先执行；新输出中的匹配随之计入。状态（`needle`、`total`、`selected`、`complete`）随帧发布；`searchPending` 在状态不属于当前查询或未完成时为真。
- 新查询找到首个匹配后选中最新匹配并滚入视口；`findNext` 经 `selectSearchMatch` 循环（向前为更旧）。空查询结束查找。
- 高亮在 `searchLayer`（选区与文字之下），颜色 `Theme.terminalSearchMatch`/`terminalSearchCurrent`。查询由 Core 保存，新 transport 的引擎沿用并重新计数。

## 5. Resize 与 ConPTY

改列宽时主屏与历史按软换行 reflow，备用屏不重排。conhost 另有一份没有历史的屏幕：libghostty 默认在光标位于末行、网格变高时从历史回拉行，conhost 不回拉，两边随即错开。因此关闭 `RESIZE_PULL_SCROLLBACK`：变高只在底部补空行，代价是放大窗口不自动露出历史。改列宽时两边各自 reflow，宽字符等边界规则不同仍可能错开；Ghostty 上游无 Windows 版本，这条路径没有上游实测。

### 5.1 尺寸变更顺序

Core 在自己的线程上先换网格，再通知 ConPTY（流程见 [ConPTY 生命周期](conpty-lifecycle.md#2-视口调整resize)）；否则新尺寸的重绘会在旧网格上解析，产生多余滚动，提示符与光标错位。resize 失败不回滚网格。回归：`terminal_session_test::resizeRepaintKeepsPromptAndCursorTogether`。

禁止的做法（都会复发问题）：

- 向 PTY 合成按键或控制字节请求重绘（如 `\x0c`）：它是真实输入，TUI 可能绑定任意功能。
- 在前端改写光标行或缓冲区：破坏 Shell 的输入状态。
- 猜测 conhost PID 注入调色板：有竞态，会使终端启动失败。
- 用多个独立定时器决定发布时机：会在一次写入中途发布半帧。

`TerminalViewportTrace`：Debug 默认开，Release 默认关，`--trace-viewport[=0|1]` 覆盖。写 `<root>/diagnostics/terminal/viewport.jsonl`，2 MiB 轮换到 `viewport.1.jsonl`；只记尺寸、非空计数、光标与节点数，不记文字或输入。

### 5.2 单元格颜色

`GhosttyEngine` 只取 `GhosttyStyle` 的语义标签：`NONE` 记为默认前景/背景，`PALETTE` 0–15 保持索引，其余与 `RGB` 解析成颜色（OSC 4/10/11 覆盖按覆盖值）。不用渲染状态已解析的 `FG_COLOR`/`BG_COLOR`，否则主题切换时屏幕与历史不重新着色。粗体、淡色、斜体、下划线样式与颜色、上划线、删除线、闪烁、反显、隐藏与超链接一并转换。历史与屏幕同一套转换。

### 5.3 重绘合并

- **resize 后**：全屏 TUI 收到新尺寸会清屏重印整段内容，按帧率呈现会看到从头滚到尾。`TerminalCoreWorker::resize` 换网格后自动进入重绘合并窗口：新网格本身立即发布；此后 750ms 内输出触发的帧等静默 50ms 再发，每段突发最多隐藏 350ms；立即发布路径不受影响。不设全局延迟下限（会拖慢打字）。
- **ED3 隐式重绘**：部分 TUI（如 omp）每次 resize 发 `CSI 2 J CSI 3 J` 后整段重印，且不带 2026；resize 窗口合并不够——下一轮 resize 会落在重印中途。Core 把 `CSI 3 J` 视为隐式同步开始（`ImplicitRedraw`）：不发布任何帧（含 `resize()` 的立即发布）；队列排空且静默 50ms 后发布，最多保持 1s；期间的 `?2026h` 转交显式事务，孤立的 `?2026l` 不结束它；传输结束或新 transport 清除。

合并与时间窗口属于呈现节奏，没有自动化测试（[测试设计规范](../../workflow/testing.md#1-测什么) 不测帧数与时序）；改动后在 [压测 TUI](lab/stress-tui.md) 或 omp 里反复拖动窗口大小，人工确认不出现从头滚到尾的重印。

## 6. 协议回复

以 libghostty 为准，不打源码补丁（见 [libghostty-vt](../../legal/libghostty-vt.md)）。下文只列本项目的接线与选择；测试在 `tests/terminal/ghostty_engine_test.cpp`。

### 6.1 单元格与模式

- 解析、字素簇（2027）、宽字符与宽度表用 libghostty 原生；ConPTY 自行排版，宽度不同的字符仍可能错位。
- DA1 用默认应答（不含 28，捆绑 ConPTY 不会改用矩形操作）。XTVERSION 回复 `SpanTui <版本>`。
- 隐藏（SGR 8）：`CellStyle::conceal` 保留到渲染，`TerminalRowLayoutBuilder` 只画背景。不能转换为"前景 = 背景"：会丢失默认色标记与调色板索引，对比度修正还会把字提亮。
- 淡色（SGR 2）在对比度修正之后向背景混合一半，否则会被修正提亮；ANSI 0–15 的下划线颜色（SGR 58）保持索引。
- 光标：用户设置的形状作为默认，`CSI 1..6 SP q` 覆盖，`CSI 0 SP q` 与复位交还；OSC 12/112 设置与恢复颜色。
- 配色报告：`CSI ? 996 n` 经 `COLOR_SCHEME` 回调按 OSC 11 覆盖值或主题背景亮度回复；`?2031h` 后主题深浅翻转时主动发送。
- DECRQM 由 libghostty 回复（含 2026）；2026 的发布时机仍由调度器按 Core 字节扫描判断。

### 6.2 Kitty 图形

- 传输、放置、删除、回复与配额由 libghostty 实现。
- `f=100` 经 `GHOSTTY_SYS_OPT_DECODE_PNG` 注入的 `QImage` 解码。
- 只接受直接传输（`t=d`），文件、临时文件、共享内存介质回复错误。
- 像素尺寸按 View 经 `TerminalSession::setCellPixelSize` 送来的格子尺寸计算。

### 6.3 OSC

- OSC 4/10/11/12 设置与查询、104/110/111/112 恢复：原生，回复与查询用同一终止符。
- OSC 9、`OSC 777;notify;…`（通知）、`OSC 9;4`（进度）、BEL 经 `TerminalHostEvent` 上报，不进帧；BEL 只在窗口不在前台时闪任务栏（去向见 [消息与任务](../application/messages-task-progress.md)）。
- OSC 133：标记过提示符后帧的 `promptMarks` 为真；`scrollToPrompt(±1)` 经 `DELTA_PROMPT` 移动视口（快捷键见 [输入协议](input-protocols.md#14-视图层约定)）。
- OSC 52 只支持写入：`CLIPBOARD_WRITE` 回调（上限 8 MiB）→ `clipboardWriteRequested` → `TerminalSession` 写剪贴板。不注册读取，避免子进程读到敏感内容。
- 标题：OSC 0/2 经 `TITLE_CHANGED` 上报，标题栈由 libghostty 维护。标题报告（`CSI 21 t`）关闭：它会把外部可控文本回显为输入。

### 6.4 XTWINOPS

`CSI 14/16/18/19 t` 经 `SIZE` 回调回复（格子尺寸由 View 每次视口提交时送到 Core）。移动、改尺寸、图标化不注册 `RESIZE_WINDOW`，静默忽略（含 ConPTY 自带的 `CSI 8 ; rows ; cols t`）。

## 7. 不支持的功能

| 功能 | 原因 |
|---|---|
| 进程重启后保留旧输出 | 新 transport 新建引擎；RIS 清空历史 |
| Sixel | 上游不支持 |
| OSC 1337 内联图片 | 上游不支持；Kitty 图形覆盖主流工具 |
| OSC 633 | 上游只识别 OSC 133；不注入 shell 集成脚本 |
| 查找的正则与全词 | libghostty 只支持字面查找 |
| Kitty 动画与虚拟放置 | 不驱动动画帧，虚拟放置不绘制 |
| 放大窗口时露出历史 | 关闭 `RESIZE_PULL_SCROLLBACK`（§5） |

## 8. 测试与基准

- 测试：`ghostty_engine_test`、`terminal_session_test`（含 `graphicsDisappearAcrossSessionBinding`、`restartReplacesTheBoundViewsGrid`）、`terminal_core_worker_test`、`terminal_view_binding_test`。行布局、字形图集、光标动画与场景图这类绘制细节不做自动化测试，由维护者人工查看。
- `tests/terminal/terminal_render_benchmark.cpp`（手动，不进 CTest）：真实 `TerminalView` + `TerminalSession` 经 `QQuickRenderControl` 在 offscreen 下渲染进 D3D11 纹理，固定节拍代替垂直同步。每行首格背景色编码行号或帧号，回读像素测量滞后、冻结与撕裂。场景覆盖滚轮与持续滚动、拖动滚动条（含边刷屏边拖的 `scrollbar-drag-flood`；测滞后用较小 `--history`，否则裁剪使行号偏移）、日志刷屏、备用屏整屏刷新、四终端同时刷新。offscreen 字体走 FreeType，结果只用于同一基准前后对比。`--json <文件>`、`--list`。
