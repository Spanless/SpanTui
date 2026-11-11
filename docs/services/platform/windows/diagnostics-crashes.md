# 启动诊断、崩溃与卡死

## 1. 启动日志（`launch_logging.cpp`）

`main.cpp` 最早期安装，记录时间、参数、运行模式、版本、DPI 与数据根迁移状态，写 `<root>\last-launch.log`。文件打不开时只写 stderr 并记警告。

### 启动失败对话框

GUI 子系统程序没有控制台，窗口出现前的失败只能用系统对话框（`src/platform/windows/native_dialog.*`，不依赖 Qt 窗口与事件循环的 `MessageBoxW`）告知：

- 内容：最近 6 条 warning/critical/fatal 原文（各截断到 500 字符，`recentLaunchProblems()`）与启动日志路径，可打开日志文件夹。
- 触发：运行模式返回非零退出码时 `reportLaunchFailure(LaunchFailure::Start)`；任何线程的 `QtFatalMsg` 在 `abort()` 前以 `Fatal` 弹出；参数非法时不碰数据根，直接显示参数错误。
- 语言：此时读不到用户语言偏好，`installLaunchLogging()` 按系统界面语言一次性解析文案，fatal 路径只读已解析文本。
- `--headless`、视觉夹具/隔离工作区（`isolatedVisualHarness()`）与 `offscreen`/`minimal` 平台不弹框。

## 2. 崩溃捕获（`crash_handler.cpp`）

- 未捕获的 C++ 异常由 `GuiApplication::notify()` 转入消息中心。
- SEH：`SetUnhandledExceptionFilter`，以 `CaptureStackBackTrace` + DbgHelp 符号化，写 `crash.log` 与 `crash-latest.log`。
- 不写 minidump：堆可能已损坏，在进程内写转储会死锁。

## 3. UI 卡死看门狗（`UiHealthMonitor`）

`src/application/diagnostics/ui_health_monitor.*`：

- UI 线程每 250ms 写原子心跳；看门狗线程每 500ms 检查，缺失 2 秒即把 Phase、epoch、窗口状态、队列深度写入 `<root>\diagnostics\hang\hang-latest.log`；恢复后追加 `recovered`，不杀进程。
- 看门狗只读预分配的原子快照，不调用可能阻塞 GUI 的 Windows API；不写 minidump（`dump=skipped-in-process`）：`MiniDumpWriteDump` 挂起全部线程并争用卡死线程可能持有的堆锁。
- 最小化时心跳 ×5、阈值同步放宽。顺序固定：先放宽阈值再放慢心跳；恢复时先补一次心跳再收紧——否则慢心跳会被误判为卡死。

## 4. 代码与测试

- `src/platform/diagnostics/launch_logging.cpp`、`src/platform/windows/{native_dialog, crash_handler}.cpp`、`src/application/diagnostics/ui_health_monitor.cpp`
- 测试：`ui_health_monitor_test`、`launch_diagnostics_test`
