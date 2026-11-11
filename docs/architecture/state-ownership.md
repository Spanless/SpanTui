# 状态所有权

每个状态只有一个 C++ 所有者；QML 只经绑定消费，不持副本。

## 1. 应用与工作区

| 所有者 | 源码 | 状态 |
|---|---|---|
| `SettingsStore` | `src/application/settings_store.*` | `settings` 表的读写与暂存队列、窗口几何、`workspace.layoutJson`、会话目录的载入与保存；不解释偏好含义 |
| `AppearanceSettings`（QML `Appearance`） | `src/application/appearance_settings.*` | 语言、主题、系统深色/减弱动效/滚轮行数、字体通道、查看器行距、`fontRevision` |
| `TerminalSettings` | `src/application/terminal_settings.*` | 终端配色方案与明暗槽位、光标、帧率、关闭瓦片默认、`shellProfiles` |
| `WorkspaceSettings` | `src/application/workspace_settings.*` | 脚本运行模式、`renderMode`、资源采样间隔、快捷键覆盖、`toolPathOverrides` |
| `ProjectWorkspaceRegistry` | `src/application/project_workspace_registry.*` | 项目列表（路径、别名、最后打开时间，持久化）；按需创建的 `ProjectContext`；向各上下文应用空闲、Git 可用与前台集合 |
| `ProjectContext` | `src/application/project_context.*` | 单项目的 `FileTreeModel`、`GitRepositoryService`、按瓦片隔离的 `DocumentManager` |
| `SessionModel` | `src/workspace/session_model.*` | 持久化的终端会话目录、各作用域当前会话、会话所选 Shell id；作用域为项目路径或 `remote:<hostId>`，统一由 `Scope`（`src/workspace/scope.h`）解析 |
| `RemoteHostRegistry` | `src/remote/remote_host_registry.*` | 保存的 SSH 主机与 DPAPI 加密凭据；本次运行的口令、已信任主机密钥、待答提示队列；每主机一个 `RemoteHostContext`（`RemoteFileModel`、`RemoteSystemMonitor`）。见 [SSH 远程主机](../services/remote/ssh-remote.md) |
| `TailscaleDeviceModel` | `src/remote/tailscale_devices.*` | 最近一次 tailnet 状态、各设备 SSH 判定、刷新代次、"仅 SSH"过滤（不持久化）；QML 单例 `TailscaleDevices` |
| `WindowTransitionCoordinator` | `src/application/window_transition_coordinator.*` | 转换阶段、代次、提交尺寸、动效冻结 |
| `WorkspaceLayout` | `src/workspace/workspace_layout.*`（网格计算在 `tile_grid.*`，存储格式在 `workspace_layout_json.*`） | 瓦片网格、侧栏宽度与折叠、右栏页、`DropSlot`、两阶段视口事务 |
| `WorkspaceController` | `src/application/workspace_controller.*` | 跨模块命令；聚焦终端的绑定作用域（`boundTerminalProjectPath` 与 `boundRemoteHostId` 互斥）；前台项目集合 `foregroundProjectPaths`（只有它们轮询 Git） |
| `Translator` | `src/application/translator.*` | 语言加载与切换 |
| `GraphicsModeService` | `src/application/graphics_mode_service.*` | 本次进程的渲染方式、原因与实际渲染器；启动标记由 `GraphicsStartupWatch` 读写 |
| `ShellProfileRegistry` | `src/application/shell_profile_registry.*` | Shell 配置列表与默认项，按 id 给出启动命令行 |
| `ToolRegistry` | `src/application/tool_registry.*` | Git、ssh/sftp、Tailscale CLI 的路径、版本、状态；可用路径发布到 `core::setToolExecutable`（`src/core/external_tools.*`） |
| `ShortcutRegistry` | `src/application/shortcut_registry.*` | 动作目录（id、分组、作用域、默认键）与绑定规则；不持用户数据 |
| `UiIconCatalog` / `FileIconTheme` | `src/application/ui_icon_catalog.*` / `src/workspace/file_icon_theme.*` | 图标目录、文件名到图标的解析 |

## 2. 编辑器、文档与 Git

| 所有者 | 源码 | 状态 |
|---|---|---|
| `DocumentManager` | `src/documents/document_manager.*` | 打开的文件、外部变更检测、缺失状态；只读 |
| `EditorController` | `src/documents/editor_controller.*` | 查找、跳转行、行 Git 状态、行模型 `CodePreviewLineModel` |
| `GitRepositoryService` | `src/git/git_repository_service.*` | History/Status/Diff 三通道调度、取消代次、缓存、历史分页、前台标志、未跟踪文件缓存 |

## 3. 终端

| 所有者 | 源码 | 状态 |
|---|---|---|
| `TerminalSession` | `src/terminal/api/terminal_session.*` | 进程与 Core 生命周期、输出队列与帧存储、最新视口与 viewport epoch、OSC 9;4 进度 |
| `TerminalSessionInput` | `src/terminal/api/terminal_session_input.*` | 粘贴与输入积压、分片定时器、括号粘贴状态 |
| `WinConPtyProcess` | `src/terminal/platform/windows/win_conpty_process.*` | ConPTY 句柄、线程池 I/O、未交付写入队列、最新待提交 resize |
| `TerminalCoreWorker` | `src/terminal/core/terminal_core_worker.*` | 独占 `GhosttyEngine`；输出消费、同步更新合并、发布时机、查找推进、transport epoch；网格变化后通知进程 resize |
| `GhosttyEngine` | `src/terminal/core/ghostty_engine.*` | libghostty 实例：屏幕、历史、视口、选区、查找结果、Kitty 图像、标题栈、提示符标记、协议模式；只在 Core 线程访问 |
| `TerminalView` | `src/terminal/render/terminal_view.*` | 键鼠与 IME 输入、Scene Graph 渲染；经 `TerminalScrollModel` 持有带小数的滚动位置；选区与查找只发请求 |
| `TerminalOutputQueue` / `TerminalFrameStore` | `src/terminal/shared/` | 跨线程输出字节与不可变帧发布、`fullRefresh` 合并、通知 ACK |

## 4. 横切服务

| 所有者 | 源码 | 状态 |
|---|---|---|
| `MessageCenter` | `src/application/message_center.*` | 持久消息、已读、Toast 栈、未处理异常拦截；独立终端只读 |
| `TaskProgressService` | `src/application/task_progress.*` | 长任务进度、160ms 延迟显示、1600ms 驻留 |
| `span::platform::process` | `src/platform/windows/process_containment.*` | 全局 Job Object、更新安装程序的脱离启动 |
| `UpdateService` | `src/update/update_service.*` | 更新源与 `[update]` 设置、镜像形态、检查结果、安装程序暂存与校验（[应用更新](../services/update/updater.md)） |
| `ProductPaths` | `src/core/product_paths.*` | 数据根、数据库、崩溃日志、临时路径 |
| `IdleGovernor` | `src/application/idle_governor.*` | "用户空闲"：窗口激活态与 30 秒输入超时，传播给 `ResourceMonitor` 与 `GitRepositoryService` |
| `ResourceMonitor` | `src/monitoring/resource_monitor.*` | 采样线程、CPU/内存/GPU 快照、可见消费者集合（无消费者不采样） |
| `UiHealthMonitor` | `src/application/diagnostics/ui_health_monitor.*` | 250ms UI 心跳（最小化时 ×5）、卡死 2 秒的诊断快照 |

## 5. 约束

1. 跨模块操作经 `WorkspaceController`（如 `FileTreeModel` 发信号 → `WorkspaceController::openFile` 校验路径并切换上下文），QML 不直接写 `DocumentManager`。
2. 异步结果校验 generation/epoch，陈旧结果丢弃。
3. QML 不持业务状态、不做持久化或跨域写。

## 6. 改动完成标准

- [ ] 新状态：确认现有所有者中没有合适归属；加入本表；有异步操作时带 generation 校验。
- [ ] 修改所有者：全库搜索读写方；信号或属性改名时同步全部 QML 绑定。
- [ ] 迁移或删除所有者：读写方全部迁移，旧类从源码与 `CMakeLists.txt` 删除；本表、`docs/ownership.json` 与服务文档已更新。
