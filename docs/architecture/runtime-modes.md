# 运行模式

`src/main.cpp` 解析参数，`src/application/runtime/` 装配对应运行时。

## 1. 完整工作区

无 `--terminal` 时的默认模式。`workspace_application.cpp` 只做边界画廊分派与单实例路由，服务图由组合根 `WorkspaceRuntime`（`src/application/runtime/workspace_runtime.*`）构造，加载 `qml/Main.qml`。

- 成员按依赖顺序声明，只引用先声明的成员；`initialize()` 完成持久化、接线与 QML 加载，`present()` 执行可见性策略。`SshAskpassBroker` 构造即监听管道；`RemoteHostRegistry` 先于控制器构造并注入。
- 析构先显式销毁 `UiHealthMonitor` 与 `QQmlApplicationEngine`，再销毁其余服务：QML 销毁过程中仍会读取 `WindowChrome`、`WindowTransition` 等单例。
- 单实例：`SingleInstanceRouter` 端点为 `"SpanTui-app-" + sha256(ProductPaths::root() + '|' + productImageName() + "|v1").left(32)`。含镜像名，因此 `SpanTui-test.exe` 不会连到已安装的 `SpanTui.exe`。主实例接收 JSON 激活请求并唤醒窗口；抢占失败的进程在有界时间内重试转发（见 [启动与单实例](../services/application/startup-single-instance.md)）。
- 已知限制：数据根不按镜像隔离，只由 `--data-dir`、便携标记与隔离参数决定（见 [路径配置](../reference/paths-configuration.md#1-数据根productpaths)）。不带这些参数的 `SpanTui-pre.exe` 会与已运行的 `SpanTui.exe` 共用 `%LOCALAPPDATA%\SpanTui`：后启动者清空内部回收站并重写 `sessions` 表。`start.bat` 与 `dev.bat run` 自动附加 `--data-dir`。

## 2. 独立终端

```cmd
SpanTui.exe --terminal --cwd "C:\projects\app" [--command "npm run dev"]
```

由 `terminal_application.cpp` 组装，加载 `qml/TerminalPanel.qml`。不走单实例；随机会话 id，只读配置，不加载项目、不读写工作区数据库；关闭窗口即终止其进程树，历史不落盘。

## 3. 边界画廊

`--boundary-gallery-shot` 或 `--boundary-gallery-theme` 时直接分派到 `runBoundaryGalleryApplication`，不创建任何领域服务（只有偏好与图标目录 `UiIcons`），只加载 `qml/BoundaryGallery.qml` 展示边界与颜色 token。

## 4. SSH askpass 辅助

`ssh.exe`/`sftp.exe` 以 `SpanTui.exe "<提示>"`（`SSH_ASKPASS`）启动本程序。`main()` 在 Qt 初始化前判断：恰一个参数且设置了 `SPANTUI_ASKPASS_PIPE`、`SPANTUI_ASKPASS_TOKEN` 时，经管道把提示交给主实例的 `SshAskpassBroker`，答复写 stdout，退出码表示成败；无窗口、不走单实例（见 [SSH 远程主机](../services/remote/ssh-remote.md)）。

## 5. 视觉夹具与诊断

- `WorkspaceVisualHarness`（`src/application/runtime/workspace_visual_harness.cpp`）：`isolateVisualHarnessPersistence()` 在 `<数据根>\isolated\run-<uuid>` 建沙箱（`.lock` 记录所属进程，退出时删除，残留由下次隔离运行清理）；按参数打开项目、文件、右栏页，可选创建 `cmd.exe /Q /K` 终端。
- 内部参数（`src/application/runtime/internal_launch_options.cpp`）：`--isolated-workspace`（`dev.bat run --smoke` 附加它并只检查进程存活）、`--workspace-shot-*`、`--resource-report <path>`（JSONL 指标），清单见 [启动参数](../reference/launch-options.md)。
