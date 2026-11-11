# 启动与单实例

## 1. 启动流程

`src/main.cpp`：

1. `parseLaunchOptions`（参数见 [启动参数](../../reference/launch-options.md)）；非法参数在建窗口前拒绝，退出码 2，交互启动时弹 [启动失败对话框](../platform/windows/diagnostics-crashes.md#启动失败对话框)。
2. 选定数据根（`ProductPaths`：`--data-dir` > 便携标记 > `%LOCALAPPDATA%`）。
3. `crash_handler::install`、`installApplicationContainment`。
4. 分派：`--terminal` → `TerminalApplication`（不走单实例）；`--boundary-gallery-*` → 边界画廊；默认 → `SingleInstanceRouter::startOrForward()`，已有主实例则转发后退出，否则运行 `WorkspaceApplication`。

单实例路由、持久化初始化、QML 加载等非零退出前同样弹启动失败对话框。

### 图形启动与软件渲染

Qt Quick 用 Direct3D 11；硬件设备或交换链创建失败时 Qt 自行改用 WARP 重试。SpanTui 另外处理：

- **仍失败**：`GraphicsStartupWatch`（`src/platform/diagnostics/graphics_startup.*`）接管主窗口 `sceneGraphError`，取代 Qt 的 `qFatal`：记 critical（原因与处理方法），以 -1 退出，由启动失败对话框显示。
- **启动中途崩溃**（多为驱动）：加载主界面前写 `<root>\<镜像名>.graphics-start`，首帧或正常退出时删除。下次启动发现标记时，`GraphicsModeService` 本次改用软件渲染并把 `renderMode` 立即存为 `software`，主界面出现后在消息中心提示可改回。无窗口、视觉夹具与独立终端模式不读写标记。
- **用户选择**：`renderMode`（`auto`/`software`）与 `--safe-graphics`（仅本次）。软件渲染经 `QSG_RHI_PREFER_SOFTWARE_RENDERER=1` 在首个窗口前设定，进程内固定。不用 Qt 的 `software` 场景图后端：终端在其上只能 CPU 逐行光栅化，而 WARP 与硬件走同一着色器路径。

首帧后启动日志记录实际渲染器（`Graphics: Direct3D 11 on <适配器> (hardware|software)`）。

## 2. 单实例路由

`SingleInstanceRouter`（`src/application/single_instance_router.*`）。

**端点名**：`"SpanTui-app-" + sha256(root.toLower() + '|' + productImageName().toLower() + "|v1").hex[0..32]`。不同数据根、不同镜像名（`SpanTui-test.exe`、`SpanTui-pre.exe`、`SpanTui.exe`）互不转发。端点隔离不等于数据隔离：不同镜像共用同一数据根时会同时成为主实例（见 [运行模式](../../architecture/runtime-modes.md)）。

**请求**：`{"version":1, "projectPath":"…", "requestId":"<uuid>"}`。同一次启动的重发共用 `requestId`；主实例记住最近 32 个，重复请求直接应答成功而不再激活。

**应答**：`{"version":1, "accepted":true, "error":""}`。发起方收到 `accepted: true` 才退出，超时或拒绝则报错。连上端点即说明有主实例，只有写入被拒才算不可达；不等待 `waitForBytesWritten()`——管道写入同步完成时它返回 false。

**争抢**：Windows 上 `QLocalServer::listen()` 不带 `FILE_FLAG_FIRST_PIPE_INSTANCE`，端点已被监听时仍会成功，不能作为独占依据。成为主实例前先创建命名互斥体 `Local\<端点名>`，已存在则放弃监听，在有限窗口内转为客户端重新转发。互斥体随进程退出（含崩溃）释放。

## 3. 代码与测试

- `src/application/launch_options.cpp`、`runtime/internal_launch_options.cpp`、`runtime/workspace_application.cpp`、`runtime/workspace_runtime.cpp`、`runtime/terminal_application.cpp`、`graphics_mode_service.cpp`
- 测试：`graphics_mode_service_test`、`launch_options_test`、`single_instance_router_test`、`workspace_session_routing_test`
