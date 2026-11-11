# 路径与配置

## 1. 数据根（`ProductPaths`）

全部运行时内容在数据根 `<root>` 下，不写注册表、Roaming `%APPDATA%` 或系统临时目录（应用内更新仅以系统临时目录作安装程序工作目录，不写文件）。数据根按序取首个命中：

| 顺序 | 条件 | 数据根 |
|---|---|---|
| 1 | `--data-dir <绝对路径>`（由此打开的独立终端继承） | 该目录，不存在则创建 |
| 2 | exe 旁有 `portable.txt` | `<exe 目录>\data` |
| 3 | 默认 | `%LOCALAPPDATA%\SpanTui`（旧数据根迁移见 [数据根](../services/storage/roots-migration.md)） |

| 内容 | 路径 | 接口 | 说明 |
|---|---|---|---|
| 数据库 | `<root>\span_tui.db` | `ProductPaths::databasePath()` | |
| INI 设置 | `<root>\settings.ini` | `ProductPaths::settingsPath()` | `[update]` 等少量设置 |
| SSH 私钥副本 | `<root>\ssh-keys\<hostId>` | `RemoteHostRegistry` | 仅当前用户可访问，删主机时删除（见 [SSH](../services/remote/ssh-remote.md#3-凭据与提示桥)） |
| 启动日志 | `<root>\last-launch.log` | `ProductPaths::launchLogPath()` | |
| 更新安装日志 | `<root>\logs\update-setup.log` | `ProductPaths::logDirectory()` | 每次覆盖 |
| 更新安装程序 | `<root>\cache\updates\` | `UpdateService` | 下次暂存时清掉其余内容 |
| 管线缓存 | `<root>\cache\<镜像名>.qtpipelinecache` | `ProductPaths::pipelineCachePath()` | 工作区读写，独立终端只读，边界画廊不用；文件不存在时不让 Qt 读取（否则告警） |
| 图形启动标记 | `<root>\<镜像名>.graphics-start` | `ProductPaths::graphicsStartMarkerPath()` | 见 [图形启动](../services/application/startup-single-instance.md#图形启动与软件渲染) |
| 崩溃日志 | `<root>\crash.log`、`crash-latest.log` | `ProductPaths::crashLogDirectory()` | |
| 回收站 | `<root>\trash` | `FileTreeModel` | 仅本次运行可撤销，启动时 `purgeTrash` 清空 |
| 卡死快照 | `<root>\diagnostics\hang\hang-latest.log` | `ProductPaths::hangDiagnosticDirectory()` | |
| 视口追踪 | `<root>\diagnostics\terminal\viewport.jsonl` | `TerminalViewportTrace` | Debug 或 `--trace-viewport` |
| 隔离运行 | `<root>\isolated\run-<uuid>` | `ProductPaths::isolatedRunsDirectory()` | `.lock` 记录所属进程，退出删除，残留由下次隔离运行清理 |

### 外部工具查找

Git、ssh/sftp、Tailscale 不随产品分发，`ToolRegistry` 在启动与"设置 → 环境 → 重新检测"时查找（`src/core/external_tools.*`）：

| 顺序 | Git、Tailscale | ssh / sftp |
|---|---|---|
| 1 | 用户指定（`tools.paths`） | 用户指定 |
| 2 | PATH 绝对条目：进程 PATH，加注册表中的系统、用户 PATH | `%SystemRoot%\System32\OpenSSH` |
| 3 | `%ProgramW6432%`/`%ProgramFiles%`/`%ProgramFiles(x86)%` 下 `Git\cmd`（Tailscale 为 `Tailscale`）；Git 另查 `%LOCALAPPDATA%\Programs\Git\cmd` | PATH 绝对条目 |

PATH 相对条目一律忽略：从资源管理器菜单启动时当前目录是被打开的文件夹，相对条目会执行其中的同名程序。

## 2. 仓库源码与构建输出目录

| 路径 | 说明 |
|---|---|
| `target/` | 已忽略；构建树与制品默认位置 |
| `target/build/` 或 `SPANTUI_BUILD_ROOT\{main,wt-<id>}` | 开发构建树（见 [构建规范](../workflow/build.md#2-输出目录与打包)） |
| `.dev/` | 已忽略；`data/` 开发数据根，`tmp/tests/<测试名>/` 测试环境重定向，`tmp/fontbuild/` 字体构建，`tools/` 辅助工具，`debug/` 手工排查 |
| `.env` | 已忽略；工具链读取的本机配置，应用不读 |
| `<构建树>/terminal-stress-tui` | 压测 TUI 独立构建树 |
| `target/release/<version>/windows-x64/` 或 `<SPANTUI_ARTIFACT_ROOT>\span-tui\<version>\windows-x64` | 打包输出，上级目录即本地更新源 |
| `assets/icons/`、`file-icons.json` | SVG 图标与文件类型映射 |
| `translations/SpanTui_zh_CN.ts` | 翻译源 |
| `docs/legal/third-party-notices.md`、`docs/legal/licenses/` | 打包生成 `THIRD_PARTY_NOTICES.md` 与 `licenses/` |
