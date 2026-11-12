# 术语表

| 术语 | 含义 | 主责文档 |
|---|---|---|
| Epoch | 视口事务版本号，每次视口提交递增 | [视口事务](../ui/layout/viewport-transitions.md) |
| generation | 单个异步请求的序列号，返回时校验，陈旧即丢弃 | [状态所有权](../architecture/state-ownership.md) |
| Phase | 视口事务阶段：`Idle` → `Preview` → `Commit` → `Idle` | [视口事务](../ui/layout/viewport-transitions.md) |
| DropSlot | 拖拽瓦片时按指针在目标列内的位置算出的插入位置 | [工作区组合](../ui/layout/workspace-composition.md) |
| ProgressOwner | `TaskProgressService` 发放的长任务进度令牌 | [消息与任务进度](../services/application/messages-task-progress.md) |
| AppPopup / PopupLayerStack | 浮层公共根 / 弹层栈，栈顶独占外点关闭 | [组件库](../ui/components/overview.md) |
| GlideHighlight | 行间滑动的共享悬停 pill | [组件库](../ui/components/overview.md) |
| CrispSurface | 物理像素对齐的 SDF 圆角表面 | [像素边界](../ui/pixel-boundaries.md) |
| PixelLine | 单物理像素分隔线 | [像素边界](../ui/pixel-boundaries.md) |
| ProductPaths | 数据根与各运行时路径 | [路径配置](paths-configuration.md) |
| WorkspaceController | 跨模块命令中心 | [状态所有权](../architecture/state-ownership.md) |
| IdleGovernor | "用户空闲"的唯一所有者 | [资源监控](../services/monitoring/resource-monitoring.md) |
| ConPTY | Windows 伪终端 API，每会话一个 Job | [ConPTY 生命周期](../services/terminal/conpty-lifecycle.md) |
| CoreWorker | 运行 `GhosttyEngine` 并发布帧的终端线程 | [终端总览](../services/terminal/overview.md) |
| GitLane | Git 只读进程车道：History、Status、Diff、Peek、DiffBody，与工作树改动探测专用的 Probe | [Git 总览](../services/git/overview.md) |
| productImageName | 单实例端点哈希的组成部分，隔离不同镜像 | [启动与单实例](../services/application/startup-single-instance.md) |
| SMOKE_OK | `dev.bat run --smoke` 通过标记 | [验证门禁](../workflow/verification.md) |
| `SpanTui-test` / `-pre` / `SpanTui.exe` | 开发镜像 / 发布候选 / 交付产品 | [分支与发布](../workflow/release.md#2-二进制身份定义) |
