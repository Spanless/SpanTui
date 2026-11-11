# 技术文档索引

开发流程与安全底线见 [AGENTS.md](../AGENTS.md)；源码路径到主责文档的映射在 [`ownership.json`](ownership.json)（`tools/check-ownership.ps1 -Files <路径>`）。

## 工作流（`workflow/`）

- [开发概览](workflow/overview.md)：仓库结构、本机配置、多工作树
- [文档与注释规范](workflow/documentation.md)
- [测试规范](workflow/testing.md)
- [验证门禁](workflow/verification.md)
- [构建与打包](workflow/build.md)
- [发布](workflow/release.md)
- [国际化](workflow/localization.md)

## 架构（`architecture/`）

- [架构总览](architecture/overview.md)：六层依赖与不变量
- [运行模式](architecture/runtime-modes.md)
- [状态所有权](architecture/state-ownership.md)
- [进程生命周期](architecture/process-lifecycle.md)
- [安全边界](architecture/security-boundaries.md)

## UI（`ui/`）

- [UI 总览](ui/overview.md)
- [设计系统](ui/design-system.md)
- [像素边界](ui/pixel-boundaries.md)
- [图标](ui/icons.md)
- [无障碍与动效](ui/accessibility-motion.md)
- [工作区组合](ui/layout/workspace-composition.md)
- [视口事务](ui/layout/viewport-transitions.md)
- [共享组件](ui/components/overview.md)

## 服务

**应用**：[启动与单实例](services/application/startup-single-instance.md) · [项目与会话](services/application/projects-sessions.md) · [设置与外观](services/application/settings-appearance.md) · [消息与任务进度](services/application/messages-task-progress.md)

**工作区**：[瓦片布局](services/workspace/layout-tiles.md) · [文件树](services/workspace/file-tree.md) · [文件查看器](services/workspace/editor.md) · [Markdown](services/workspace/markdown.md)

**远程**：[SSH 远程主机](services/remote/ssh-remote.md)

**Git**：[总览](services/git/overview.md) · [状态、历史与 Diff](services/git/reads-history-diff.md)

**终端**：[总览](services/terminal/overview.md) · [ConPTY 生命周期](services/terminal/conpty-lifecycle.md) · [输入协议](services/terminal/input-protocols.md) · [渲染与滚动历史](services/terminal/rendering-scrollback.md) · [独立终端与脚本](services/terminal/standalone-script-launch.md) · [压测 TUI](services/terminal/lab/stress-tui.md)

**存储**：[数据根迁移](services/storage/roots-migration.md) · [工作区数据库](services/storage/database-schema.md) · [文档标签](services/storage/documents-recovery.md)

**Windows 平台**：[无边框窗口](services/platform/windows/window-chrome.md) · [右键菜单](services/platform/windows/explorer-integration.md) · [进程包含](services/platform/windows/process-containment.md) · [诊断与崩溃](services/platform/windows/diagnostics-crashes.md)

**监控与更新**：[资源监控](services/monitoring/resource-monitoring.md) · [应用更新](services/update/updater.md)

## 参考与法律

- [术语表](reference/glossary.md) · [启动参数](reference/launch-options.md) · [路径配置](reference/paths-configuration.md)
- [第三方许可](legal/third-party-notices.md) · [libghostty-vt](legal/libghostty-vt.md) · [ConPTY](legal/conpty.md)
