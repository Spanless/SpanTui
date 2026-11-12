# 开发工作流概览

仓库结构、环境准备与并行开发。约束见 [AGENTS.md](../../AGENTS.md)。

## 1. 仓库结构

根目录只有下表条目；`tools/check-layout.ps1` 以本表为准，未被忽略且不在表中的顶层条目报错。

<!-- repo-layout -->
| 路径 | 作用 |
|---|---|
| `AGENTS.md` | 开发入口 |
| `README.md`、`README.zh-CN.md` | 产品说明 |
| `CONTRIBUTING.md`、`SECURITY.md`、`CODE_OF_CONDUCT.md`、`CHANGELOG.md` | 贡献、安全、行为准则、变更记录 |
| `LICENSE` | MIT 许可证（第三方许可见 `docs/legal/`） |
| `CMakeLists.txt` | 顶层 CMake 工程 |
| `dev.bat` | 开发入口：构建、测试、运行、打包、诊断、清理 |
| `start.bat`、`package.bat`、`tui_test.bat` | 双击入口，转调 `dev.bat` 或 `tools/` |
| `.env.example` | 本机配置模板 |
| `.gitignore`、`.gitattributes` | 忽略规则、换行与二进制属性 |
| `.editorconfig`、`.clang-format` | 编辑器默认值、C++ 格式（只查改动行） |
| `.githooks/` | 提交前、推送前钩子 |
| `.github/` | CI、分支规则、模板、CODEOWNERS、Dependabot |
| `src/` | C++17 源码，每个子目录一个静态库 |
| `qml/` | QML 表现层 |
| `shaders/` | 着色器 |
| `assets/` | 图标、字体、品牌素材 |
| `translations/` | 翻译源 |
| `installer/` | Inno Setup 脚本、资源管理器扩展注册 |
| `tests/` | 自动化测试，镜像 `src/` |
| `tools/` | 工具链、检查脚本、资源生成器、压测 TUI |
| `docs/` | 技术文档 |
| `target/` | 已忽略：构建树与制品 |
| `.dev/` | 已忽略：运行、测试、工具与排查产出 |
| `.env` | 已忽略：本机配置 |

卫生规则（`check-layout` 检查可机械判断的部分）：

1. 构建树与制品进 `target/` 或 `.env` 指定目录，其余产出进 `.dev/` 子目录（见 [路径配置](../reference/paths-configuration.md#2-仓库源码与构建输出目录)）；不建其他根目录，不写系统 TEMP、`%APPDATA%`。
2. 不建联接或符号链接；改输出位置用 `SPANTUI_BUILD_ROOT`、`SPANTUI_ARTIFACT_ROOT`。
3. 本机路径只写在 `.env`；受管文件不含机器绝对路径。
4. `.gitignore` 只管本项目产出与通用编辑器目录；个人工具目录写进 `.git/info/exclude`。
5. 做法替换时同轮删除旧路径、旧参数、旧忽略规则。

## 2. 环境准备

1. 按 [构建规范](build.md#0-环境要求) 安装 Qt 6.12 MinGW、CMake、Ninja；Qt 不在 `C:\Qt` 时在 `.env` 设 `SPANTUI_QT_SDK_ROOT`。
2. `dev.bat build`，再 `dev.bat run --smoke`。
3. `git config core.hooksPath .githooks`；从 `dev` 切功能分支（见 [分支模型](release.md#1-分支模型)）。

## 3. 并行开发（多工作树）

每项任务一个工作树，不在同一检出里切分支或用 stash 搬运改动。

```cmd
:: 在 <主检出目录名>.worktrees\feat-xxx 检出 feat/xxx（不存在时从 dev 新建）
dev.bat worktree add feat/xxx
dev.bat worktree remove feat/xxx
```

- 各工作树独立：分支、构建树（`SPANTUI_BUILD_ROOT` 下 `wt-<id>`，未设时为自身 `target/build`）、`.dev/`、`dev` 命令互斥量；单实例按数据根区分。
- 共用：主检出的 `.env`（工作树自有 `.env` 时逐项优先）、制品目录、sccache 缓存。
- 首次构建接近全量编译。
- `remove` 只删干净的工作树；外部建的或已手工删除的用 `dev worktree prune` 收回；`dev worktree list` 列出全部。

## 4. 本域改动完成标准

### 构建脚本（`CMakeLists.txt`、`dev.bat`、`tools/devtool/`、`tools/toolchain.ps1`）
- [ ] 新测试目标出现在 `dev.bat test --list -R <名称>`。
- [ ] `dev.bat check` 输出 `CHECK_OK`；改到的 `dev.bat` 子命令实际跑通一次。
- [ ] [构建规范](build.md)、[验证门禁](verification.md) 的命令与输出标记与实现一致。

### 顶层目录或产出位置
- [ ] §1 与 [路径配置](../reference/paths-configuration.md) 已同步，旧位置规则已删，`check-layout` 通过。

### 静态检查（`tools/check-*.ps1`、`tools/agent-check.ps1`）
- [ ] 含中文的脚本为 UTF-8 BOM，Windows PowerShell 5.1 可运行。
- [ ] 新规则已用临时违规样例验证，样例已删。

### 协作约束（`AGENTS.md`、`docs/ownership.json`、本目录）
- [ ] `tools/check-ownership.ps1` 通过。
- [ ] 每条约束只在一个主责文档展开。
