# ConPTY 生命周期

`WinConPtyProcess`（`src/terminal/platform/windows/win_conpty_process.*`）。

## 1. 创建

- **管道**：`\\.\pipe\SpanTui-<pid>-<counter>-<in|out>`，DACL 只授予当前用户 SID（`OwnerOnlyPipeSecurity`）。
- **命令行**：由调用方给出（工作区会话来自 `ShellProfileRegistry`，见 [Shell](../application/settings-appearance.md#shell)）；未给出时取 `terminal::detectShells()` 首项（`pwsh` → `powershell` → `cmd`）。
- **挂起创建**：`CREATE_SUSPENDED` 启动，恢复前把 Shell 与控制台宿主加入会话专用 Job（`assignNewChildrenToJob`）。
- **环境**：剥离 `WT_SESSION` 等变量，避免子进程误判为 Windows Terminal。`StartOptions::environment` 在其后叠加（同名覆盖），目前只用于远程会话的 askpass 变量与 `TERM`（见 [SSH 远程](../remote/ssh-remote.md)），不持久化。

### 读取与写入边界

没有专属线程。两端管道以重叠模式打开，各绑一个线程池 I/O 对象（`CreateThreadpoolIo`）；进程退出用 `RegisterWaitForSingleObject` 等待。读完成回调用 `PeekNamedPipe` 带上已到达的字节（这些补读不进线程池），单块最多 64 KiB，再发起下一次读。管道读空时以 `endsWrite = true` 交给 `OutputCallback`，读满而仍有数据时为 `false`。写入在锁内排成 FIFO（上限 1 MiB），同一时刻一个重叠写在途，完成回调发下一段；`write` 只入队，从不阻塞调用方。conhost 每帧写一次，故块边界即帧边界；`TerminalOutputQueue` 保存边界，Core 只在边界发布（见 [渲染 §1](rendering-scrollback.md#1-场景图渲染与快照交换)）。偶发的偏早边界只退化为逐块发布，不丢字节。

### 捆绑 ConPTY

- `conPtyApi()` 首次使用时加载程序目录下的 `conpty.dll`（`ConptyCreatePseudoConsole` 等），它启动同目录 `OpenConsole.exe`，使各 Windows 版本用同一宿主；来源见 [ConPTY](../../legal/conpty.md)。结果缓存，DLL 不卸载。
- 回退系统 `CreatePseudoConsole`：`--conpty=system` 或 DLL 不存在（info）；`OpenConsole.exe` 缺失、加载失败、缺导出（warning）。`OpenConsole.exe` 缺失时不加载 DLL——它会静默改用系统 `conhost.exe`。
- `usesBundledConPty()` 报告当前选择。构建树与便携目录都把两个文件放在可执行文件旁。

## 2. 视口调整（Resize）

`TerminalView::commitViewport` 分配新视口 epoch 并调 `TerminalSession::commitViewport`；Session 记下最新视口，按顺序投递到 Core：

```text
Session.requestViewport(columns, rows, epoch)   GUI：过期 epoch 丢弃，同网格同 epoch 忽略
  → Core.resize(columns, rows, epoch)           Core 线程：先换网格并发布
       网格变了 → 进入重绘合并窗口，process.requestResize(columns, rows)
  → WinConPtyProcess：记下最新尺寸；空闲时提交一次线程池回调
       回调循环取最新尺寸 → ResizePseudoConsole，直到没有新尺寸
```

1. **先网格后后端**：后端的重绘可能先于 `ResizePseudoConsole` 返回到达，Core 在同一线程先换网格再通知进程（原因见 [渲染 §5.1](rendering-scrollback.md#51-尺寸变更顺序)）。
2. **最新值合并**：后端同一时刻只有一个 resize 回调在途，期间到达的尺寸只保留最后一个。
3. **失败只记录**：`ResizePseudoConsole` 失败经 `qWarning` 进入 `MessageCenter`；不重试、不回滚网格，下次视图换尺寸时再提交。
4. **启动中**：Session 立即记下新视口并让 Core 换网格；进程启动成功后 `attachProcess` 若发现网格与启动尺寸不同，补发一次 resize。
5. **只换 epoch**：同网格新 epoch 只让 Core 以新 epoch 重发帧，不通知进程。
6. **不合成输入**：不向 PTY 写 `Ctrl+L`、清屏序列等请求重绘。
7. **清屏**：Core 本地清屏后，Session 调进程 `clearScreen` → `ConptyClearPseudoConsole(hpc, TRUE)`（只写信号管道，不等待）。系统 ConPTY 无此导出，只清本地。

conhost 在 resize 后只重绘可见区（`CSI H` + 逐行 `CSI K`，放大时带 `CSI 8;rows;cols t`），不重放历史。"resize 后内容从头滚到尾"来自全屏 TUI 自己清屏（含 ED3）重印，Core 的合并策略见 [渲染 §5.3](rendering-scrollback.md#53-重绘合并)。

## 3. 退出与销毁

- **自然退出**：记录 `exitCode`，不再接受按键；`restartSession()` 以相同参数重启。
- **强制销毁**：关闭会话 Job 句柄（`JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE`）终止进程树，对仍存活的 Shell 再 `TerminateProcess`。
- **关闭顺序**（`Impl::shutdown`）：先停写入与待处理 resize，关闭 Job 并终止仍存活的 Shell；再 `CancelIoEx` 两端管道并 `WaitForThreadpoolIoCallbacks` 等回调退出，关闭 I/O 对象后才关管道句柄，最后 `ClosePseudoConsole`、`UnregisterWaitEx(INVALID_HANDLE_VALUE)` 等退出回调结束。这样不会对已关闭、可能被复用的句柄读写；读端先于 `ClosePseudoConsole` 关闭，conhost 不会卡在写满的管道上。
- **退出通知**：退出等待回调先关写入与伪控制台，等读完成回调读到管道末尾，再调 `ExitCallback`，保证退出排在最后一段输出之后。
- **回调寿命**：线程池回调只用 `Impl` 指针，`shutdown` 返回时它们都已结束；resize 回调另持 `shared_ptr<Impl>`。`resize` 与 `ClosePseudoConsole` 在同一把锁下互斥。

## 4. 代码与测试

- `src/terminal/platform/windows/win_conpty_process.cpp`；捆绑 ConPTY：`src/terminal/vendor/conpty/`，更新 `tools/update-conpty.ps1`。
- 测试：`win_conpty_process_test`、`terminal_session_test`。
