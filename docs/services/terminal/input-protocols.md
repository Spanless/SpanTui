# 终端输入协议与 IME

## 1. 键盘

### 1.1 两种输入

- **按键（`TerminalKeyStroke`）**：一次物理按下/重复/抬起。`TerminalInputRouting::translateKey` 从 `QKeyEvent` 取两种身份：VT 协议用的逻辑身份（命名键 `key`，或字符键的 `text` 与 Kitty 备用键 `textKey`；AltGr 已折叠进字符），与 Windows 按键记录 `TerminalNativeKey`（虚拟键、扫描码、未整形字符如 Ctrl+A 的 U+0001、控制键状态）。控制键状态取自 `nativeModifiers()`，AltGr 表现为左 Ctrl + 右 Alt。`VK_PROCESSKEY`、`VK_PACKET` 不算物理按键。
- **文字**：输入法提交、`TerminalSession::sendText`、括号粘贴正文。文字从不做键盘协议编码。

两者经 Session → Core 分别进入 `GhosttyEngine::sendKeyStroke`/`sendText`，在 Core 线程决定字节。输入使离开底部的视口回到底部。

### 1.2 通道选择

ConPTY 把输入交给两类子进程：读按键记录的经典控制台程序（PSReadLine、cmd、crossterm 系 TUI），与开启 VT 输入的程序（Node 系 TUI、WSL、ssh）。`GhosttyEngine` 依次判断：

1. **子进程协商了 VT 键盘协议**（当前屏 Kitty 标志非零或 modifyOtherKeys > 0）：VT 编码。只有 VT 输入程序会协商，conhost 原样透传。
2. **conhost 请求了 win32-input-mode（DECSET 9001）**：有物理身份的按键按 `KEY_EVENT_RECORD`（`TerminalWin32Input::keyRecords`）发 `CSI Vk;Sc;Uc;Kd;Cs;Rc _`，按下抬起各一条，多 UTF-16 单元的字符追加字符记录。无物理身份的字符键（无 Ctrl/Alt/Meta）按文字发送，其余无物理身份的按键走 VT。
3. **其余**：VT 编码。

文字编码：通道 2 下 ASCII 原样发送，其余字符逐个 UTF-16 单元发无虚拟键的记录对；其他通道发 UTF-8。

libghostty 不识别 9001 且不回调未知 CSI，由 CoreWorker 的字节扫描（同时识别 2026 与 ED3）识别 `CSI ? 9001 h/l` 与 XTMODKEYS 4，在该字节之后设置到引擎；新 transport 新建引擎即重置。9001 下的按键效果尚待维护者在真实 ConPTY 中验证。

### 1.3 VT 编码

- 交给 libghostty 按键编码器，每次编码前 `ghostty_key_encoder_setopt_from_terminal` 同步终端模式。支持 Kitty Keyboard Protocol 与 modifyOtherKeys，未协商时输出传统 xterm/DEC 序列；细则以 libghostty 为准。
- 命名键映射到 libghostty 键码（小键盘带 `keypad` 标记）；字符键带布局文字与未加 Shift 的码位；C0 控制字符不作为文字传入。
- Shift 不算修饰：Shift 产生的字符作为文本发送，只有 Ctrl/Alt/Meta 使文本键进入 Kitty 消歧或 modifyOtherKeys。
- 不上报单独的修饰键（视图不转发）。
- 编码只依据协商结果，不按子进程名分支。

### 1.4 视图层约定

- **只转发终端输入**：单独修饰键与 Win 组合键不转发，交给 QML 快捷键。
- **按下与抬起成对**：按扫描码记录已转发的按下，抬起只在其按下已转发时转发。输入法消费的按下、视图自己处理的按下（复制、中断）没有抬起；失焦或换绑会话时清空记录。否则经典程序会读到未按下的键被抬起。
- **`Qt::ImEnabled`**：只在有渲染消费者且会话运行时为真。Qt 只在焦点变化与显式 `update(Qt::ImEnabled)` 时读取，为假时 Windows 摘除 IME 上下文，输入法热键（Ctrl+Space）会作为普通按键进入子进程。因此绑定会话、会话启停、渲染消费者变化时都要通知 Qt 重新读取。
- **快捷键先于 PTY**：`TerminalView.shortcutBindings`（动作 id → 按键序列，由应用层 `ShortcutRegistry` 提供）在转发前匹配，命中的按下被消费。同一组合可绑多个动作，按 id 顺序执行全部适用的；`terminal.copy` 仅在有选区时适用，`terminal.previousPrompt`/`nextPrompt`（默认 Ctrl+Shift+Up/Down）仅在帧带 OSC 133 标记时适用，其余总是适用。都不适用时按普通按键转发——因此 Ctrl+C 绑复制时，有选区复制、无选区仍中断。`terminal.copy`/`paste` 在视图内执行，其余经 `shortcutActivated(id)` 交给 QML。AltGr 与 IME 组合期间不匹配；会话已退出仍匹配；自动重复只消费不重复执行。
- **Ctrl+C**：固定行为，不进快捷键表。有选区复制；无选区撤销排队输入并发 ETX（`\x03`）。
- 提示符跳转、光标形状、BEL 见 [渲染 §6](rendering-scrollback.md#6-协议回复)。

### 1.5 ConPTY 输入特性（实测）

- 对经典程序，conhost 把 VT 翻译回按键，且有损：modifyOtherKeys 与 CSI-u 被丢弃，Shift+Enter 与 Enter 不可分，Ctrl+Space 的 NUL 变为 Shift+Ctrl+2。
- UTF-8 写入的非 ASCII 字符中，能映射到 ANSI 代码页且键盘上无键位的（`“ ” ‘ ’` 等）被合成为 Alt+小键盘序列，crossterm 系 TUI 会丢弃。
- 按键记录无损：经典程序收到原样记录；Node 子进程收到 conhost 翻译的标准序列，字符记录还原为 UTF-8；记录与原始 VT 字节可混发。
- conhost 不把记录翻译成 Kitty 编码，所以协商后改走 VT。Ctrl+Space 在两种通道都无法作为 NUL 交给 Node 子进程。

## 2. 粘贴、鼠标与写入

- **括号粘贴**：子进程开启 `?2004h` 时正文包裹 `\e[200~`/`\e[201~`，按文字发送。
- **鼠标**：libghostty 鼠标编码器按子进程的跟踪模式与格式（含 SGR 1006、像素）编码；焦点上报（1004）同样由它处理。横向滚轮编码为按键 6/7。未开启跟踪时拖动为本地选区，开启时按住 Shift 仍走本地选区。
- **备用屏滚轮**：未开鼠标上报且在备用屏时，纵向滚轮按系统滚动行数换算成方向键按下/抬起（`TerminalInputRouting::alternateScrollKeys`，带虚拟键与扫描码）；按住 Shift 仍滚动视图。
- **选区**：见 [渲染 §4.1](rendering-scrollback.md#41-选区与复制)。

### 写入与取消

- `TerminalSessionInput` 在 GUI 侧持粘贴与输出积压，有界分片，超限报警。
- Session 直接调进程的 `write`，成功只表示后端接管字节，调用不阻塞。进程启动中或后端拒收（未交付超过 1 MiB）时 `TerminalSessionInput` 保留当前写入，稍后重试，后续普通写入不得越过。
- 后端未交付预算 1 MiB，含排队与在途写入。`cancelPendingWritesAndInterrupt`、停止与析构在锁内丢弃排队字节；已进入 `WriteFile` 的数据无法撤回。`pendingWriteBytes()` = 排队 + 在途未写完的字节。

## 3. IME 锚点

`TerminalCursorResolver`（`src/terminal/render/terminal_ime_anchor_resolver.cpp`）决定候选窗位置：

- 协议光标可见：用光标位置（`ProtocolCursor`）。
- 光标隐藏（全屏 TUI 常用反显自绘光标）：扫描屏幕，恰好一个单格反显区时用它（`ReverseVideoCell`）；零个或多个时保持上一次锚点。

## 4. 代码与测试

- 编码：`src/terminal/core/ghostty_engine.cpp`、`terminal_win32_input.cpp`；9001/XTMODKEYS 识别：`terminal_core_worker.cpp`（`SynchronizedUpdateParser`）。
- IME：`src/terminal/render/terminal_ime_anchor_resolver.cpp`、`terminal_ime_composition_state.cpp`。
- 测试：`ghostty_engine_test`、`terminal_input_routing_test`、`terminal_session_test`、`win_conpty_process_test`。
