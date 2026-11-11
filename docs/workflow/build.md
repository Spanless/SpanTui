# 构建与打包

## 0. 环境要求

| 依赖 | 要求 |
|---|---|
| Windows | 10 1809（17763）x64 及以上 |
| Qt | 6.12.x MinGW 64-bit，含 Shader Tools、SVG；建议装 TaskTree（缺少时每次配置有一条无害的 CMake 警告）。不支持 MSVC |
| MinGW | Qt 安装器附带的 13.1 64-bit |
| CMake / Ninja | CMake ≥ 3.22、Ninja |
| PowerShell | Windows PowerShell 5.1 |
| clang-format（可选） | 与 CI 同主版本，位于 `PATH` 或 `%ProgramFiles%\LLVM\bin`；缺失时提交前格式检查只给 WARN |
| 打包（可选） | Inno Setup 6、Windows SDK（MakeAppx、SignTool） |

`tools/toolchain.ps1` 解析工具链，优先级：

1. `SPANTUI_QT_ROOT`、`SPANTUI_CMAKE`、`SPANTUI_MINGW_BIN`、`SPANTUI_NINJA_DIR` 逐项覆盖；
2. 在 `SPANTUI_QT_SDK_ROOT`（默认 `C:\Qt`）下发现最新 `mingw_64` 套件与 `Tools\` 中的工具；
3. `PATH`。

缺项时报错并列出变量，不回退到本机路径。变量可写在仓库根 `.env`（`KEY=VALUE`，`#` 注释），进程环境优先；应用本身不读 `.env`。

其他变量：

| 变量 | 作用 |
|---|---|
| `SPANTUI_ISCC` | Inno Setup 不在默认位置时 |
| `SPANTUI_MSIX_CERT_THUMBPRINT` | 证书库有多张 `CN=Span` 证书时选定一张 |
| `SPANTUI_LINKER` | 如 `lld`：以 `-fuse-ld=<值>` 换链接器；依次在 `SPANTUI_LINKER_DIR`、`PATH`、`%ProgramFiles%\LLVM\bin` 查找，所在目录置于构建 `PATH` 最前，找不到报错 |
| `SPANTUI_DEBUG_INFO` | `OFF` 时所有构建类型 `-g0`；默认 `ON` |

后两项作为同名 CMake 选项传给每棵构建树，变化后自动重新配置。CI 设 `SPANTUI_LINKER=lld`、`SPANTUI_DEBUG_INFO=OFF`。

CI 与发版的 Qt、MinGW、clang-format 版本固定在 `tools/toolchain-versions.json`；`dev.bat release` 核对本机版本，不一致即拒绝发版。

## 1. dev.bat 命令

`dev.bat` 只启动 `tools/devtool/dev.ps1`，后者在当前控制台调用对应 worker。同一检出的命令按检出路径的互斥量串行；不同工作树可并行（见 [并行开发](overview.md#3-并行开发多工作树)）。

```cmd
dev.bat build                         :: 增量构建主程序
dev.bat test -R a -R b                :: 构建并运行选中测试（-R 取并集，省略为全量）
dev.bat test -R x_test --function f --repeat 10   :: 只跑一个测试函数，首次失败即停
dev.bat test --coverage               :: 插桩构建并输出覆盖率
dev.bat run --smoke                   :: 无窗口冒烟
dev.bat package                       :: 便携包与安装程序
dev.bat release [--dry-run]           :: 对 CI 测过的 master 头发布 vX.Y.Z
dev.bat diag [<探针>]                 :: 性能诊断
dev.bat clean                         :: 删除 .dev\
dev.bat check                         :: 只跑检查脚本
dev.bat ci [--core]                   :: 本地复现 CI
dev.bat worktree add|list|remove|prune
```

- 何时构建与测试见 [验证门禁](verification.md#1-验证分级)。不直接调用 `cmake --build`/`ctest`：手工调用跳过构建树配置校验与目标映射。
- 开发构建产出带调试符号的 `SpanTui-test.exe`；打包以 `SPANTUI_INSTALLED_IMAGE=ON` 构建 `SpanTui-pre.exe`，分发时改名 `SpanTui.exe`（见 [二进制身份](release.md#2-二进制身份定义)）。
- 工具只停止当前构建树内的 `SpanTui-test.exe`/`SpanTui-pre.exe`，按完整镜像路径匹配——本机可能同时运行已安装的 `SpanTui.exe`。
- `start.bat`、`dev run`、smoke、diag、`tui_test.bat` 都带 `--data-dir <仓库>\.dev\data`（调用方已给出时不追加）。`start.bat` 原样转发应用参数。

### dev test

- `dev.bat build` 不刷新 `*_test.exe`；测试只经 `dev.bat test` 构建。
- 流程：构建 `build.ninja`（使新注册测试可见）→ `ctest -N --show-only=json-v1` 枚举 → 把测试命令映射回 CMake 目标（未构建过的测试按同名目标，否则 `all`）→ 只构建这些目标 → `ctest --output-on-failure --parallel <N>`（`N` 取 `--jobs`，默认处理器数）。输出 `TEST_OK`/`TEST_FAIL`。
- 测试环境设 `QT_FORCE_STDERR_LOGGING=1`（无控制台时 QtTest 日志会进 `OutputDebugString`），并把 `TEMP`/`TMP`/`APPDATA`/`LOCALAPPDATA` 重定向到 `.dev\tmp\tests\<测试名>\`，每次运行前重建（见 [测试规范](testing.md#4-测试环境隔离与安全约束)）。
- `--build-only`（`TEST_BUILD_OK`）、`--list`；`--function` 要求 `-R` 恰好选中一个测试，worker 按 CTest 的 `ENVIRONMENT_MODIFICATION` 设环境后直接运行镜像；`--repeat N` 连跑到首次失败；`--label <标签>` 与 `-R` 取并集。
- `dev ci --core [--base <修订>]`：以与 `origin/dev` 的合并基点为默认基准列出改动文件（含未跟踪），经 `docs/ownership.json` 换算成测试正则，并入 `core` 标签；涉及 `src/core/`、`src/platform/`、构建脚本、`tools/` 或测试支持文件时改跑全量。
- `--coverage`：在构建树 `coverage/` 子树以 `SPANTUI_COVERAGE=ON` 构建（`-fprofile-update=atomic`，否则多线程计数为负、gcovr 拒绝解析），运行前清除 `.gcda`，报告写入 `coverage/report/`（`index.html`、`summary.json`、按库汇总的 `layers.txt`；只统计 `src/`，排除 vendor）。gcovr 装在 `.dev\tools\gcovr` 虚拟环境，缺失时报错并给出安装命令。
- 测试程序设 `QT_QML_MODULE_NO_IMPORT_SCAN`（`spantui_skip_qml_import_scan`）：共享 Qt 下 `qmlimportscanner` 不链接任何东西，却占重新配置的绝大部分时间。不经 `spantui_add_library_test` 注册的测试程序也要调用它。

### 构建树配置

开发树、打包树与压测 TUI 都经 `Initialize-SpanTuiBuildTree` 配置：完整 CMake 参数记在 `.spantui-configure`，参数相同且是 Ninja 树时复用，否则重新配置。生成器不是 Ninja，或缓存的 `Qt6Core_DIR` 不在当前 Qt 套件下时，删除 `CMakeCache.txt` 与 `CMakeFiles/` 后重配——只改 `Qt6_DIR` 会保留旧套件的各模块路径。不让 IDE 复用此构建树。

### 静态库分层

`src/CMakeLists.txt` 自底向上声明静态库，链接边即分层约束：

| 静态库 | 源码 | 直接依赖 |
|---|---|---|
| `SpanCore` | `src/core/` | Qt Core（PUBLIC 统一 `_WIN32_WINNT`）；数据库迁移私有链接 Qt Sql |
| `SpanPlatform` | `src/platform/`（Shell 扩展 DLL 除外） | SpanCore |
| `SpanTerminal` | `src/terminal/` | SpanCore、SpanGhosttyVt（预编译静态库，构建不需要 Zig，见 [libghostty-vt](../legal/libghostty-vt.md)） |
| `SpanMonitoring` | `src/monitoring/` | SpanCore（要测量的会话由宿主注入，见 [资源监控](../services/monitoring/resource-monitoring.md)） |
| `SpanGit` | `src/git/` | SpanCore、SpanPlatform（工作区递归监视 `DirectoryChangeWatcher`） |
| `SpanDocuments` | `src/documents/` | SpanCore（含编辑器控制器与行模型） |
| `SpanWorkspace` / `SpanUpdate` | 同名目录 | SpanCore、SpanPlatform（Workspace 另链 `SpanUiIcons`） |
| `SpanRemote` | `src/remote/` | SpanCore、SpanPlatform、Qt Gui/Network/Sql |
| `SpanApplication` | `src/application/`（`runtime/` 除外） | 以上全部 |

`SpanTui` 只编译 `src/main.cpp` 与 `src/application/runtime/`。含 QML 可见类的库在 CMake 末尾调用 `qt_extract_metatypes(<库>)`，供 `SpanTui` 的 qmltyperegistrar 读取 `runtime/qml_types.h` 中的 `QML_FOREIGN` 声明；新库向 QML 暴露类时同样补上。`SpanTui` 的 QML 模块声明 `DEPENDENCIES QtQuick` 并直接链接 Qt Gui/Quick：暴露的类派生自 `QQuickItem` 或使用 `QFont`，缺了这两项编译器解析不了这些类型，相关绑定全部退回解释执行。QML 编译覆盖情况用 qmllint 的 `--compiler warning` 检查（约定见 [架构总览](../architecture/overview.md)）。测试经 `spantui_add_library_test`（终端为 `spantui_add_terminal_test`）链接被测库。跨层引用先调整分层，不在下层链接上层。

## 2. 输出目录与打包

构建树与制品默认在已忽略的 `target/` 下。可选变量（相对值从主检出根算起）：

| 变量 | 未设置时 | 设置后 |
|---|---|---|
| `SPANTUI_BUILD_ROOT` | 各检出 `target/build` | 主检出 `<根>\main`，工作树 `<根>\wt-<id>` |
| `SPANTUI_ARTIFACT_ROOT` | `target\release\<version>\windows-x64` | `<根>\span-tui\<version>\windows-x64`，各检出共用，勿并行打包同一版本 |
| `SPANTUI_WORKTREE_ROOT` | `<主检出目录名>.worktrees` | 新工作树位置 |

- 构建树内含 `SpanTui-test.exe`、测试、`diag/`、`terminal-stress-tui/` 与打包用 `release/` 子树。
- `.dev/`：`data/`（开发数据根）、`tmp/` 等（测试重定向）、`tools/`（辅助工具）、`debug/`（手工排查）。`dev.bat clean` 停止本工作区镜像后删除整个 `.dev/`。
- 压测 TUI 是独立工程，由 `tools/terminal_stress_tui/build.ps1` 构建，不经 `dev.bat`（见 [压测 TUI](../services/terminal/lab/stress-tui.md)）。
- 每个版本一个制品文件夹，重打包替换同版本；其上级目录即应用内本地更新源（见 [应用更新](../services/update/updater.md#1-更新源检测与确认流程)）。旧版本不自动删除。整包在 `<制品目录>.staging` 组装，全部成功后才替换。

`package-worker.ps1` 步骤（任一步失败即中止；`--clean` 全量重建，`--reconfigure` 强制重配）：

1. 非 `--quick` 时先确认 Inno Setup、MakeAppx、SignTool 与证书齐全。
2. 在 `release/` 子树构建 `SpanTui-pre.exe` 与 `SpanTui-shell-extension.dll`，以 `--headless --isolated-workspace --data-dir <仓库>\.dev\data` 冒烟。
3. 组装便携目录：`SpanTui.exe`、Shell 扩展、ConPTY（`conpty.dll`、`OpenConsole.exe`）、`shell/` 注册脚本、`THIRD_PARTY_NOTICES.md`、`licenses/`（含 Qt SBOM）、`portable.txt`（便携标记，见 [路径配置](../reference/paths-configuration.md#1-数据根productpaths)，安装程序排除）。每次重跑 `windeployqt`，跳过 `platforminputcontexts`（Qt Virtual Keyboard 仅 GPL）并排除 SQLite 以外的 SQL 驱动；部署结果含 Virtual Keyboard 文件即失败。不传 `--no-virtualkeyboard`：模块未安装时该参数报 `Unknown option`。写 `spantui-build.json`（版本、构建时间、主程序名）。
4. 非 `--quick` 时生成并签名稀疏身份包 `shell/SpanTui-shell-identity.msix`、导出公钥证书、编译安装程序。
5. 用 `%SystemRoot%\System32\tar.exe` 打 `SpanTui-<版本>-portable.zip`。
6. 写 `release-manifest.json`（版本、`builtAtUtc`、各文件大小与 SHA-256），替换制品目录。

## 3. 诊断探针（`dev diag`）

不属于验证门禁，按需运行。

```cmd
dev.bat diag              :: 列出探针
dev.bat diag idle-render  :: 待机渲染：帧数、脏节点、线程 CPU → PERF_IDLE_OK|WARN
dev.bat diag memory       :: 内存时间线：WorkingSet、Private、句柄、GDI/USER 对象
```

- 探针即 `tools/devtool/diag/<名称>.ps1`，必须声明 `-Root -BuildDir -AppPath -OutDir`；共享能力在 `diag/common.ps1`。新增探针不需要改 `dev.ps1`。
- 输出到 `<构建树>\diag\<探针>-<时间戳>\`（`report.txt` 与原始数据）；退出码 `0`=OK，`1`=WARN。
