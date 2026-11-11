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
- `qml_style_check`（`tools/check-style.ps1`）：规则见 [设计系统](../ui/design-system.md)，特例用 `// style-ok: 原因` 豁免。
- `translation_catalog_test`：见 [国际化](localization.md)。

## 3. 静态检查

`tools/agent-check.ps1` 按改动文件运行 QML 样式、C++ 格式、`check-docs`、`check-layout`、`check-ownership -Changed`（列出应读文档、高影响对象、收尾检查；代码改了而主责文档未动时 WARN）与 `git diff --check`，成功输出 `AGENT_CHECK_OK`。

- `.githooks/pre-commit` 运行 `agent-check -Staged`：改动清单、格式、ownership、空白取暂存内容，其余读工作树；暂存区为空时直接通过。启用：`git config core.hooksPath .githooks`（agent 不自行修改 git 配置）。
- `.githooks/pre-push` 拒绝直接推送 `master`、`dev`，其余推送先跑 `dev.bat check`（`CHECK_OK`）。
- C++ 格式：`tools/check-format.ps1` 按 `.clang-format` 只查新增或修改的行（新文件查全文），`-Fix` 就地格式化；`src/terminal/vendor/` 保持上游格式。本机无 clang-format 或主版本与 `tools/toolchain-versions.json` 不同时给 WARN。

## 4. CI（`.github/workflows/ci.yml`）

触发：指向 `dev`、`master` 的 PR（必需检查 `windows`）、合并后的推送、手动 `gh workflow run ci.yml --ref <分支>`。同一 PR 或分支的新运行取消旧运行。

- 进 `dev` 的 PR：`dev.bat ci --core --base <目标分支>`（`core` 标签加受影响测试，规则见 [构建规范](build.md#dev-test)）。
- 其余运行：全量 `dev.bat ci`。合并后的全量运行暴露 PR 未覆盖的回归。

任务：

- `reuse`（Linux）：草稿 PR 不构建；被测 tree 与某个已通过的推送运行相同时沿用其结果（dev → master 的 PR 与合并提交）；只改 `docs/` 与根目录 `.md` 的 PR 只跑 `dev.bat check`。出错时照常构建。
- `version`（Linux，仅 dev → master）：`tools/check-version.ps1`（见 [发版](release.md#4-发版)）。
- `windows`（`windows-2022`）：安装固定版本的 Qt、sccache、lld 后运行 `dev.bat ci`；进 `dev` 的 PR 先从 PyPI 装固定版本 clang-format 跑 `check-format.ps1 -Base <基准> -Require`（runner 自带 LLVM 版本不同）。sccache 缓存整目录经 actions/cache 传递，只有 dev 的推送运行保存——每份约 2 GB，PR 保存的归档只对本 PR 可见并会挤占配额。

action 固定到提交 SHA（行尾注释标签），Dependabot 每月更新；`reuse`、`version` 限时 10 分钟，`windows` 90 分钟。
