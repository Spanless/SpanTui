# 验证门禁

## 1. 验证分级

构建很慢，动态验证集中在任务收尾做一次。

| 级别 | 操作 | 时机 |
|---|---|---|
| 静态检查 | 阅读、搜索、`git status/diff/log`；`tools/agent-check.ps1`、`tools/check-*.ps1`、`git diff --check` | 随时，免授权；改动中唯一允许的验证 |
| 收尾验证 | 一次增量 `dev.bat build`；`dev.bat test -R <受影响测试>`；列出时加 `dev.bat run --smoke`（均取自 `check-ownership.ps1 -Changed`） | 完成时，免授权；失败先修完全部再重跑 |
| 需授权 | 窗口验证、安装依赖、`dev.bat package`、提交与其他 Git 写操作 | 逐条授权，不覆盖相邻指令 |

- 全量 `dev.bat test` 只在维护者要求时跑；`dev.bat ci` 可在推送前自愿运行。只改文档时静态检查通过即可。
- 排查失败测试只跑出错的函数：`dev.bat test -R <测试> --function <函数> [--repeat N]`。

## 2. 门禁要求

- `build`：增量；无构建树损坏证据时不全量重建。
- `run --smoke`：须得到 `SMOKE_OK`。`package` 在 smoke 通过后执行。
- 测试正则合并为一次 `dev.bat test -R a -R b`；不写 `"a|b"`——PowerShell 调 `.bat` 时去掉引号，cmd 把 `|` 当管道。
- 窗口验证由维护者完成；未经要求不开可见窗口、不截图（`--workspace-shot`/`--boundary-gallery-shot` 同）。交付时写明未做视觉验收及需查看的界面。
## 3. 静态检查

`dev.bat check`（CI 与 pre-push 同）运行 `check-docs`、`check-layout`、`check-style`、`check-translations`、`check-icons`，成功输出 `CHECK_OK`；它们不注册为 CTest 测试。

- `tools/check-style.ps1`：QML 设计 token 规则见 [设计系统](../ui/design-system.md)，特例用 `// style-ok: 原因` 豁免。
- `tools/check-translations.ps1`：译文目录与源码逐条对应，见 [国际化](localization.md)。
- `tools/check-icons.ps1`：图标名唯一、SVG 与动效文件可加载、QML 与文件图标映射引用的名字都有 SVG，见 [图标](../ui/icons.md)。

`tools/agent-check.ps1` 按改动文件运行 QML 样式、译文（`qml/`、`src/`、`translations/`）、图标（`qml/`、`assets/icons/`）、C++ 格式、`check-docs`、`check-layout`、`check-ownership -Changed`（列出应读文档、高影响对象、收尾检查；代码改了而主责文档未动时 WARN）与 `git diff --check`，成功输出 `AGENT_CHECK_OK`。

- `.githooks/pre-commit` 运行 `agent-check -Staged`：改动清单、格式、ownership、空白取暂存内容，其余读工作树；暂存区为空时直接通过。启用：`git config core.hooksPath .githooks`（agent 不自行修改 git 配置）。
- 两个钩子启动 Windows PowerShell 5.1 前清除 `PSModulePath`：pwsh 7 的模块路径经 git 这类非 PowerShell 进程传下来后，5.1 会加载 pwsh 的 `Microsoft.PowerShell.Utility`，`Get-FileHash` 等脚本定义的 cmdlet 随之失效。pwsh 7 直接启动 5.1 时自己会清理，所以只在这条链路上出现。
- `.githooks/pre-push` 拒绝直接推送 `master`、`dev`，其余推送先跑 `dev.bat check`（`CHECK_OK`）。
- C++ 格式：`tools/check-format.ps1` 按 `.clang-format` 只查新增或修改的行（新文件查全文），`-Fix` 就地格式化；`src/terminal/vendor/` 保持上游格式。本机无 clang-format 或主版本与 `tools/toolchain-versions.json` 不同时给 WARN。

## 4. CI（`.github/workflows/ci.yml`）

触发：指向 `dev`、`master` 的 PR（必需检查 `windows`）、合并后的推送、手动 `gh workflow run ci.yml --ref <分支>`。同一 PR 或分支的新运行取消旧运行；`master` 除外，以免中途取消发版留下草稿。

- 进 `dev` 的 PR：`dev.bat ci --core --base <目标分支>`（`core` 标签加受影响测试，规则见 [构建规范](build.md#dev-test)）。
- 其余运行：全量 `dev.bat ci`。合并后的全量运行暴露 PR 未覆盖的回归。

任务：

- `reuse`（Linux）：草稿 PR 不构建；被测 tree 与某个已通过的推送运行相同时沿用其结果（dev → master 的 PR 与合并提交）；只改 `docs/` 与根目录 `.md` 的 PR 只跑 `dev.bat check`。出错时照常构建。
- `version`（Linux，仅 dev → master）：`tools/check-version.ps1`（见 [发版](release.md#4-发版)）。
- `windows`（`windows-2022`）：安装固定版本的 Qt、sccache、lld 后运行 `dev.bat ci`；进 `dev` 的 PR 先从 PyPI 装固定版本 clang-format 跑 `check-format.ps1 -Base <基准> -Require`（runner 自带 LLVM 版本不同）。构建后打印 Ninja 日志的耗时分布（按步骤类别汇总与最慢的 20 步），调优读这里，不为测量额外触发运行。
  - Qt 与 MinGW 由本地复合 action `.github/actions/setup-qt` 准备（`release` 任务共用）：整目录一份 actions/cache 归档（约 420 MB），键由固定版本与模块组成；命中时不再运行安装 action——它即使命中自带缓存，也要先装新 Python 并从源码编译 aqtinstall 的依赖，每次约一分钟。未命中时 action 以 `set-env: false`、`add-tools-to-path: false` 安装后保存；两种情况都只靠 `SPANTUI_QT_SDK_ROOT` 找套件，构建环境一致。
  - sccache 缓存整目录经 actions/cache 传递（无调试信息时每份约 40 MB）。dev 的推送运行与 PR 运行都保存：PR 先恢复本 PR 上次推送的归档，没有时取目标分支的，所以修复后再推只重编改动部分。PR 之间互相看不到归档：dev 的推送运行保存之前开的 PR 会全量编译一次。配额 10 GB，超出时 GitHub 先淘汰最久未访问的；dev 最新的归档每个新 PR 都会读取，不会先被淘汰。
  - sccache 版本固定在 `tools/toolchain-versions.json`（action 默认取最新发布）。CI 设 `SPANTUI_SCCACHE=external`：构建脚本不另起本机那种按仓库分的服务，用 action 起的服务与恢复的目录。命中率看 "Compiler cache statistics" 步骤；action 的收尾注释关闭——它统计的是我们的服务停止后新起的空服务，总显示 "0 hits"。
  - rcc 把每个资源文件的修改时间写进生成的 qrc 源码，而每次检出的时间都不同，qrc 目标（字体资源单个编译 10–16 秒）因此永远不命中缓存。CI 设 `QT_RCC_SOURCE_DATE_OVERRIDE` 为常量时间，使其可复现；只在测试任务设置，`release` 任务不设。
- `release`（`windows-2022`，仅推送到 `master`）：`windows` 通过或被 `reuse` 跳过后，在环境 `release` 中导入签名证书并运行 `dev.bat release`，打包、签名并发布 `vX.Y.Z`（见 [发版](release.md#4-发版)）。不用 sccache，Release 构建冷编译。

action 固定到提交 SHA（行尾注释标签），Dependabot 每月更新；Linux 任务写明镜像版本（`ubuntu-24.04`），不用会随时迁移的 `ubuntu-latest`。`reuse`、`version` 限时 10 分钟，`windows` 90 分钟，`release` 120 分钟。
