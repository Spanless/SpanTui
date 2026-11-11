# 应用更新

`UpdateService`（`src/update/update_service.*`，QML 单例 `AppUpdate`）是更新状态的唯一所有者。文件替换、占用处理与卸载登记交给 Inno Setup，产品不自带替换程序。

## 1. 更新源、检测与确认流程

同时只用一个源：

| 源 | 生效条件 | 检测 | 安装前准备 |
|---|---|---|---|
| 远程（默认） | 未开启本地更新 | `GET https://api.github.com/repos/Spanless/SpanTui/releases/latest` | 下载 → 校验 |
| 本地 | 开发者页开启「从本地构建更新」 | 读本地文件夹中最新版本的 `release-manifest.json` | 复制 → 校验 |

切换源或修改本地文件夹会取消进行中的检查并清掉结果；已确认的安装按原源执行到底。检查与下载按 `m_generation` 丢弃过期结果。

### 远程

- 稳定版（`GitHubRelease`）：非 `draft`、非 `prerelease`，标签为 `vX.Y.Z`（`v` 可省、无后缀），按数字比较高于 `applicationVersion`。404 视为尚无稳定版的成功检查。
- 取首个名为 `SpanTui-<版本>-Setup.exe` 的资产（`InstallerSource::isInstallerName()`），没有即失败。便携 zip 不参与。
- 下载到 `<root>\cache\updates\<资产名>`（`QSaveFile`，60 秒无数据超时）。请求带 `User-Agent: SpanTui/<版本>`，15 秒超时，不带凭据。

### 本地

- 源是 `dev package` 各版本文件夹的**上级**（`target\release` 或 `<制品根>\span-tui`，见 [构建](../../workflow/build.md#2-输出目录与打包)），一次设好不随版本变化。文件夹 URL 经 `setLocalSourceFolderUrl()` → `QUrl::toLocalFile()`，网络共享保留服务器名。
- `InstallerSource::probe()`（工作线程）读各子文件夹 `windows-x64\release-manifest.json`，按版本再按 `builtAtUtc` 取最新；无法解析的跳过。最新包未列出安装程序（`--quick`）或文件不存在时检查失败——旧包不顶替有问题的最新包。
- 版本相同时比较清单与运行镜像 `spantui-build.json` 的 `builtAtUtc`（ISO 8601 文本比较），清单更晚才算更新；镜像无构建戳时以 `applicationVersion` 为准。

### 镜像形态（`InstallerSource::imageKind()`）

| 形态 | 判定 | 发现更新后 |
|---|---|---|
| 安装版 | 有 `unins*.exe` 且无 `portable.txt` | 暂存、校验、静默安装（§2） |
| 便携版 | 有 `portable.txt` | 只提示，「打开下载页」（本地源打开最新版本文件夹） |
| 开发构建 | 其他 | 同便携版 |

界面经 `canInstall`、`portable`、`downloadPageUrl` 区分；不能安装时 `applyAndRestart()` 直接返回假。

### 设置与确认

- `settings.ini` 的 `[update]`：`localSourceEnabled`、`localSourcePath`、`autoApply`（均默认关/空）。
- 发现更新只提示（侧栏圆点、设置通用页）；用户点「更新并重启」并在确认框（标明来源）确认后才开始。
- `autoApply`（仅安装版）：启动 1.5 秒后的静默检查（无窗口模式不检查）与本地文件夹变更后的检查发现更新即安装。
- 检查与安装经 `TaskProgressBinding`（`Update`）上报；安装总进度按步骤权重合成（下载 8、复制 2、校验 2、启动 1）。失败经 `updateFailed` 进消息中心，更新保持可用以便重试。

## 2. 暂存、校验与静默安装

步骤（`m_applySteps`）：远程「下载 → 校验 → 启动」，本地「复制 → 校验 → 启动」，无摘要时省去校验。下载在主线程异步；复制、校验、启动各是一个工作线程任务（`QtConcurrent::run` + `QPromise`，1 MiB 分块），GUI 线程不做文件或进程操作。

1. **暂存**：安装程序一律放进 `<root>\cache\updates\` 再运行，本地源所在网络共享断开也不影响。
2. **校验**：SHA-256 对比来源摘要（GitHub 资产 `digest` / 清单 `sha256`），不符即删除并失败；GitHub 无摘要时跳过。摘要与安装程序同源，只防损坏，不防篡改。
3. **清理**：删除 `cache\updates\` 中除本次安装程序外的一切；刚运行过的安装程序被占用，留到下次删除。
4. **启动**：`startUnaffiliatedProcess`（`CREATE_BREAKAWAY_FROM_JOB`）脱离应用 Job，工作目录为系统临时目录；脱离失败即报错，不降级为 Job 内启动。未签名安装程序首次运行要经安全扫描，`CreateProcess` 可能阻塞数秒，故也在工作线程。成功后主程序立即退出。

`UpdateService` 析构时取消工作线程任务，关闭窗口后不会再启动安装程序。

### 进度卡片

`UpdateProgressDialog` 在 `applyStarted` 时由 `Main.qml` 打开，只读 `AppUpdate` 的 `applying`、`applySteps`、`applyStep`、`applyStepProgress`（0–1，未知为 -1）、`applyDetail`（按 0.1% 或 0.1 MB 步长更新）、`applyError`（下次安装开始时清空）。「隐藏」只关卡片，安装继续。

### 安装程序参数（`installerArguments` ↔ `installer/SpanTui.iss`）

| 参数 | 作用 |
|---|---|
| `/SILENT /SUPPRESSMSGBOXES /NORESTART` | 只显示安装进度窗口 |
| `/CLOSEAPPLICATIONS` | 经 Restart Manager 关闭占用文件的程序 |
| `/WAITPID=<pid>` | `InitializeSetup` 先等该进程退出（至多 90 秒，超时交给 Restart Manager） |
| `/RELAUNCH=1` | 完成后由 `[Run]`（`Check: RelaunchRequested`）以原用户身份启动新版 |
| `/LOG=<root>\logs\update-setup.log` | 每次覆盖；无法创建日志时 Setup 中止，故宿主先建好 `logs\` |

安装程序侧：

- 沿用上次的安装目录、任务选择与权限模式；装在 `Program Files` 的副本静默更新仍会弹 UAC。
- 先以 `trust-machine.ps1 -Mode Check`（只读证书存储）确认右键菜单证书已受信任，已受信任则不再提权。
- 资源管理器加载了右键菜单 DLL 时，Restart Manager 会重启资源管理器。
- `[InstallDelete]` 清理旧版更新器残留：安装目录的 `SpanTui-updater.exe`、同级 `.spantui-stage-*`/`.spantui-backup-*`、默认数据根的 `cache\updates\package\` 与 `logs\updater.log`。

## 3. 代码与测试

- `src/update/{update_service, github_release, installer_source}.cpp`；`installer/SpanTui.iss`、`installer/shell-extension/trust-machine.ps1`
- QML：`GeneralSettingsSection.qml`、`DeveloperSettingsSection.qml`、`Main.qml`（`confirmUpdate()`）、`business/update/UpdateProgressDialog.qml`
- 测试：`update_service_test`（以 `file://` 夹具离线运行，`argv_echo_child` 冒充安装程序核对步骤、进度与命令行）。安装程序脚本没有自动化测试，需在真实安装上验收（见 [发布](../../workflow/release.md)）。
