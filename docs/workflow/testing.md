# 测试设计规范

## 1. 测什么

测试只覆盖用户会遇到的行为：坏了用户会看到错误结果、丢数据、卡住、残留进程、无法启动或更新。写之前先回答"这条测试失败时，用户遇到的是什么 bug"，答不出就不写。以下不写成测试：

- **检查脚本**：翻译目录、图标引用、样式与文档规则属于静态检查 `tools/check-*.ps1`，不注册 CTest（见 [验证门禁](verification.md)）。
- **读像素与量绘制**：颜色值、坐标与几何、行布局、字形图集、动画曲线、帧数与呈现节奏。界面由维护者在真实窗口或边界画廊查看。
- **内部机制**：线程何时启动、写入是否批量、模型角色名、默认值从哪读、增量更新的粒度、内部队列与缓存——用户可见结果不变时实现可以自由改，测试不应因此失败。

断言对象是状态所有者的公开可观察结果：公开方法返回值、模型 `data()`、用户可见事件的信号及参数、SQLite 行、文件系统副作用、进程树与 Job 归属、终端网格内容、Git 工作区/Index/HEAD、`OperationOutcome` 错误枚举。

## 2. 反模式

以下写法在行为坏掉时仍会通过：

1. 扫描源码文本（断言代码长什么样）。
2. 像素比对或 UI 点击回放。
3. 在测试里复制产品算法再比较。
4. 注入私有成员或写死后端返回值。
5. 用例体内写 `for`/`while`/`if`/`switch`：失败看不出是哪一轮，分支让断言悄悄跳过。多组输入用 `_data` 表；带控制流的逻辑放进匿名命名空间的辅助函数，由用例比较其返回值（如 `QCOMPARE(explicitBackgrounds(frame), QStringList{"0:0 [X] 0"})`），辅助函数不计算期望值。例外：紧跟 `QSKIP(...)` 的单行 `if (!条件)`。`init`/`cleanup`/`initTestCase` 与 `_data` 属于夹具。

**属性测试**：解析器、编解码器等"任意输入都成立"的不变量用 `tests/support/property_check.h`：`span::tests::firstCounterexample(种子, 用例数, 生成, 判定, 描述)` 返回首个反例或空串，用例写 `QCOMPARE(..., QString())`；`SPANTUI_PROPERTY_SEED` 换种子或重放。另提供 `tokenString`、`randomCuts`/`piecesAt`、`hexOf`。

**变异检查**：怀疑覆盖不足时手工改坏逻辑，只跑覆盖它的测试确认失败后还原；按需进行，不进 CTest。

**计时**：
- 产品计时器经 `...ForTesting` 设置器缩短，只改时长不改行为。
- 等用户可见的结果出现，不断言时间窗口本身。
- `QTRY_*` 每次重查前睡 50ms；等待频繁的 git 测试用 `git_test_support.h` 的 `GIT_TRY_*`（约 10ms）。

## 3. 测试注册

- 根 `CMakeLists.txt` 的 `SPANTUI_CORE_TESTS` 带 CTest 标签 `core`，标准是"坏了会怎样"：终端不可用、用户数据丢失或损坏、残留进程、无法启动或更新、告警丢失。新测试默认不进核心；列表含未注册名时配置失败。本地 `dev.bat test --label core`。
- 新测试用 `spantui_add_terminal_test` 或 `spantui_add_library_test` 链接被测库，并确认 `dev.bat test --list -R <名称>` 可见——未注册的测试从不运行。不再使用的测试文件直接删除。
- 纯 QML 交互没有自动化入口：smoke 覆盖加载，交互由维护者验证，交付时列出操作路径。
- 性能基准（`terminal_core_worker_benchmark`、`terminal_render_benchmark`）用 `qt_add_executable` 构建但不注册 CTest，手动运行。
- `workspace_startup_smoke`（仅 Debug）：以 `--workspace-shot` 在 offscreen 下启动完整工作区，用隔离数据目录在 `.dev\tmp` 打开真实 ConPTY（`GIT_CEILING_DIRECTORIES` 阻止 git 上爬进本仓库），`--workspace-shot-delay-ms=500` 后退出。只断言启动与正常退出，截图不比对。
- `WorkspaceRuntime` 的接线抽成应用层函数（如 `connectSessionTerminals`、`connect*Messages`），组合根与测试调用同一份。
- CTest 只注册产品测试。翻译目录与图标引用的完整性由 `tools/check-translations.ps1`、`tools/check-icons.ps1` 检查，和其他静态检查一起在 `dev.bat check`、CI 与提交前运行。

## 4. 测试环境隔离与安全约束

`tests/` 镜像 `src/`。开发机上通常运行着真实的 SpanTui，测试不得触及用户环境。

| 领域 | 要求 |
|---|---|
| 写出位置 | `spantui_set_test_environment` 把 `TEMP`/`TMP` 设为 `.dev\tmp\tests\<测试名>\temp`，`APPDATA`/`LOCALAPPDATA` 设为同级 `appdata`、`localappdata`；每个测试独占（测试并行运行，固定文件名会互相覆盖）。`spantui_add_library_test` 已调用；直接 `add_test` 的须自行调用。不用 `QStandardPaths` 的其他位置。 |
| 配置与数据库 | `QTemporaryDir` 或 `ProductPaths::setRootOverride`；不用默认 `QSettings`（写注册表）。 |
| Git | 只用临时目录内的本地 bare 仓库作远端。测试环境清除 `GIT_DIR`、`GIT_WORK_TREE`、`GIT_INDEX_FILE` 等变量：git 钩子导出它们，子进程继承后会改写运行钩子的检出。无 `git.exe` 时 `QSKIP`。 |
| SSH | 不连真实服务器：askpass 管道进程内往返；远程脚本在 Git for Windows 的 `sh.exe` + GNU `find` 上只操作 `mktemp -d` 目录，缺失时 `QSKIP`；凭据只用临时数据库。 |
| 终端与 ConPTY | 用专用夹具（如 `pty_echo_child.exe`）。夹具回显走 UTF-16 控制台 API（CP 437 下窄字符读写会把中文变 `?`）；标准输出全缓冲，每条回复一次 flush（逐次写入在 CPU 争用下每次往返上百毫秒，多行回复会超时）。输入前同时等 `running()` 与就绪标记（`waitForChildOutput`）：首批输出可能早于运行态通知到达，未运行时 `sendText`/`sendKeyStroke` 丢弃输入。分片场景显式逐块 `WriteFile`。 |
| 进程 | 断言父子 PID 与 Job 归属。 |
| 仓库文件与构建产物 | 不以编译定义传入源码或构建树路径（sccache 在检出间共享对象，见 [编译缓存](build.md#编译缓存)）：要用的仓库文件以资源嵌入（字体用 `spantui_add_font_resources`，取 `:/fonts/...`），构建产物按 `QCoreApplication::applicationDirPath()` 查找。 |
| 窗口与焦点 | 创建窗口的测试以 `OFFSCREEN` 注册（`spantui_add_terminal_test` 默认带上），`QT_QPA_PLATFORM=offscreen`，`QT_QPA_FONTDIR` 指向系统字体目录（offscreen 默认只扫 `<Qt>/lib/fonts`）。依赖原生窗口的断言改测 Qt 公开查询（如焦点对象的 `Qt::ImEnabled`）。 |
