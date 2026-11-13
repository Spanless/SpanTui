# 分支与发布

## 1. 分支模型

| 分支 | 作用 | 发布 |
|---|---|---|
| `dev` | 默认分支；功能分支从此切出并以 PR 合回 | beta（`vX.Y.Z-beta.N`） |
| `master` | 稳定线；只接受来自 `dev` 的 PR | 稳定版（`vX.Y.Z`） |
| 功能分支 | `feat/…`、`fix/…`、`chore/…`，合并后删除 | — |

- `master`、`dev` 不能直接推送、强推或删除；发版 tag `v*` 建立后不能删除或移动（发版任务新建 tag 不受限）。规则在 `.github/rulesets/dev-branch.json`、`master-branch.json`、`release-tags.json`，用 `gh api repos/{owner}/{repo}/rulesets --method POST --input <文件>` 启用（GitHub 只对公开或付费私有仓库执行）；本地由 `.githooks/pre-push` 拦截。
- 合入 `dev` 用 squash，`dev` 合入 `master` 用合并提交；规则集的 `allowed_merge_methods` 各只允许一种。
- `.github/CODEOWNERS` 自动请求维护者审查；不要求批准数。

### Pull Request 流程

目标是各 PR 的 CI 并行运行、互不排队。

1. 每个指向 `dev`、`master` 的 PR 必须通过 `windows`，进 `master` 还须通过 `version`（CI 内容见 [验证门禁](verification.md#4-cigithubworkflowsciyml)）。
2. 不要求分支与 dev 同步（`strict_required_status_checks_policy: false`），否则每次合并都迫使其余 PR 重跑。两个各自通过的 PR 合并后可能失败，由第 3 步兜底。
3. 合并进 `dev` 后跑全量；dev 变红时先 `git revert` 引入问题的提交（另开 PR），再修。这次运行也负责更新 dev 的编译缓存。
4. dev → master 的 PR 与合并提交 tree 与 dev 已通过的提交相同，`reuse` 跳过构建。合并前先在 dev 准备好版本（§4）；合并即发版。

## 2. 二进制身份定义

| 文件 | 配置 | 用途 |
|---|---|---|
| `SpanTui-test.exe` | Debug / RelWithDebInfo，`SPANTUI_INSTALLED_IMAGE=OFF` | 开发镜像，含测试夹具 |
| `SpanTui-pre.exe` | Release，`SPANTUI_INSTALLED_IMAGE=ON` | 打包前的候选镜像 |
| `SpanTui.exe` | Release | 交付给用户的安装版与便携版 |

## 3. 便携版目录与发布清单（`release-manifest.json`）

便携目录包含：`SpanTui.exe`、`SpanTui-shell-extension.dll`、`THIRD_PARTY_NOTICES.md`、`licenses/`（含 `licenses/Qt/` SBOM，见 [第三方声明](../legal/third-party-notices.md)）、`conpty.dll`、`OpenConsole.exe`（见 [ConPTY](../legal/conpty.md)）、`spantui-build.json`（版本印章，更新检查据此判断本机版本），以及 Qt 6 与 MinGW 运行库和 QML 模块；不含仅以 GPL 提供的 Qt 模块。

最低系统 Windows 10 1809（17763）x64；Windows 11 ARM64 经 x64 模拟运行。安装程序以 `MinVersion` 强制；更旧系统缺 `CreatePseudoConsole`。

制品根还有 `SpanTui-<版本>-portable.zip`（根目录为 `SpanTui-portable\`）与 `release-manifest.json`（版本、`builtAtUtc`、所有交付文件的大小与 SHA-256）。本地更新源按最新版本的清单定位并校验安装程序（见 [应用更新](../services/update/updater.md#1-更新源检测与确认流程)）。

GitHub 稳定版（`vX.Y.Z`，非 pre-release）必须含 `SpanTui-<版本>-Setup.exe`：安装版只从"最新稳定版"的该资产静默更新；便携版只打开发行页。beta 发布为 pre-release，不推送给用户。

## 4. 发版

dev → master 的 PR 合并后，master 的推送 CI 在测试通过（或 `reuse` 沿用 dev 的结果）后由 `release` 任务自动运行 `dev.bat release`；维护者只需在 dev 上准备好版本再合并。自动发版失败时，修复原因后在 Actions 页重跑该任务，或在本机对 master 头手动运行 `dev.bat release`（需本机证书库有同一张签名证书）。`dev.bat release` 依次：

1. HEAD 为 `origin/master` 且工作树干净（本机可 `git switch --detach origin/master`），该提交在 master 的推送 CI 已成功（CI 内运行时由任务依赖保证）。
2. `tools/check-version.ps1` 通过；本机 Qt、MinGW 与 `tools/toolchain-versions.json` 一致。
3. 执行与 `dev.bat package` 相同的打包，核对 `release-manifest.json` 与 `spantui-build.json` 版本均为 `X.Y.Z`。
4. 以草稿创建 `vX.Y.Z`，上传 Setup、portable zip、`release-manifest.json`，说明取 `CHANGELOG.md` 该版本段落；全部上传后转正式并标为最新。中途失败只留草稿，删除后重跑。

版本校验（CI `version` 任务与 `dev release` 都运行）。版本唯一来源是 `CMakeLists.txt` 的 `project(SpanTui VERSION X.Y.Z)`：

- 格式 `X.Y.Z`；
- `assets/brand/` 各应用清单的 `assemblyIdentity version` 为 `X.Y.Z.0`（windres 原样嵌入，需手工同步）；
- `CHANGELOG.md` 有非空 `## [X.Y.Z]`，`## [Unreleased]` 已清空；
- 远端无 `vX.Y.Z` 标签，且高于所有稳定版标签（更新检查只提供更高版本）。

### 签名证书

资源管理器菜单的身份包用自签名 `CN=Span` 证书签名，安装程序把它的 `.cer` 按指纹加入 `LocalMachine\TrustedPeople`（换证书后已安装版本下次更新会请求一次提权）。证书由维护者在本机一次性准备：

```powershell
tools\create-shell-signing-certificate.ps1 -BackupDir <仓库外的安全目录> -Publish
```

- 在 `Cert:\CurrentUser\My` 建（或复用）私钥可导出的证书，本机 `dev package`/`dev release` 直接使用；旧版脚本建的不可导出证书加 `-Replace` 换掉。
- 向 `-BackupDir` 写备份：`SpanTui-shell-signing.pfx`、密码 `SpanTui-shell-signing-password.txt`、公钥 `.cer`。目录必须在仓库外（`dev clean` 会删 `.dev\`），自行妥善保管。
- `-Publish` 建 GitHub 环境 `release`，部署分支策略只放行 `master`，并把 PFX（base64）与密码存为该环境机密 `SHELL_SIGNING_PFX`、`SHELL_SIGNING_PFX_PASSWORD`。PR 运行的 ref 不是 `master`，即使改了工作流引用该环境也拿不到机密。需要对仓库有管理员权限的 `gh auth login`。

CI 的 `release` 任务把 PFX 导入 runner 的证书库、以指纹交给打包，结束时删除。

`--dry-run` 在任意提交上演练：逐条报告 `BLOCKED:` 但不停止，照常打包核对，只打印 `gh` 命令；全部通过输出 `DRY_RUN_OK`，否则 `DRY_RUN_BLOCKED` 并非零退出。beta 不由 `dev release` 发布。

## 5. 发布验收

- [ ] 工作树干净，全部 CTest 通过，`SpanTui-pre.exe` 得到 `SMOKE_OK`。
- [ ] 在装有上一稳定版（启用资源管理器菜单）的机器上，经本地更新源完成一次应用内更新：进度依次走完复制、校验、启动且界面不卡；无 UAC；Setup 只显示安装进度；结束后重启到新版本，"应用和功能"版本已变。
- [ ] 安装程序与便携包已生成到制品目录。
- [ ] `licenses/` 与 `THIRD_PARTY_NOTICES.md` 覆盖包内全部第三方文件。

相关源码：`tools/devtool/package-worker.ps1`、`tools/devtool/release-worker.ps1`、`tools/create-shell-signing-certificate.ps1`、`.github/workflows/ci.yml`（`release` 任务）、`installer/`、`tools/write_build_stamp.cmake`。
