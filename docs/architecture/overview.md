# 架构总览

Qt 6（C++17 / QML）Windows 开发工作区与终端。

## 1. 分层

每个 `src/<域>/` 是一个静态库（见 [静态库分层](../workflow/build.md#静态库分层)），只依赖更下层，同层互不依赖；第 4 层各库只依赖第 1–2 层。

```text
6  QML 表现层            qml/：Main.qml、business、common/base、Theme
       ↑ 声明式 QML 单例与类型（runtime/qml_types.h）、槽调用
5  应用接线层            src/application/：runtime/（WorkspaceRuntime、TerminalApplication）、
                         Preferences（SettingsStore + 三个偏好对象）、WorkspaceController、MessageCenter、TaskProgress、
                         IdleGovernor、WindowTransitionCoordinator、diagnostics/
4  领域服务              workspace、documents（含编辑器）、git、update、monitoring、remote
3  终端核心              src/terminal/：TerminalSession、CoreWorker、GhosttyEngine、FrameStore、
                         WinConPtyProcess、矢量网格渲染
2  平台适配              src/platform/：WindowChrome、ProcessContainment、CrashHandler、LaunchLogging、DirectoryChangeWatcher
1  基础设施              src/core/：ProductPaths、ProductIdentity、OperationOutcome、工作区数据库迁移
```

约束：

1. 第 1–4 层不依赖 `src/application/` 与 QML 类型；领域事件到应用概念的映射在第 5 层接线处完成（如 Git 任务信号只带操作名，由 `WorkspaceRuntime` 接到任务进度）。
2. QML 只消费 C++ 注册的服务与模型，不管理数据库、进程、解析器或文件；common/base 的 UI 偏好只读 `Appearance` 单例。
   - 注册是声明式的：`src/application/runtime/qml_types.h` 在可执行文件里用 `QML_FOREIGN` 为各库的类声明 QML 名（首字母大写的单例，如 `WorkspaceController`、`Shortcuts`、`TaskProgress`），构建期由 qmltyperegistrar 读取，QML 编译器据此把绑定编成 C++、qmllint 可校验。领域库不含 QML 宏，只在 CMake 里 `qt_extract_metatypes`。
   - QML 在组件间传递的对象（`ProjectContext`、`FileTreeModel`、`DocumentManager`、`GitRepository`、`GitItemModel`、`RemoteHostContext`、`RemoteFileModel`、`RemoteSystemMonitor`、`TerminalSession`）同样有不可创建的 QML 名，属性按类型声明而非 `var`。可默认构造的被注册类不能是 `final`：Qt 的注册模板会从它派生。
   - 为让编译器而非解释器执行绑定：面向 QML 的 `Q_PROPERTY` 一律 `FINAL`（否则访问被视为"可被遮蔽"而退回解释）；`Q_PROPERTY`/`Q_INVOKABLE` 中命名空间内的类型写全限定名；含委托或 `Component` 的 QML 文件用 `pragma ComponentBehavior: Bound`，委托以 `required property` 取 `index`/`model`/`modelData` 与角色；函数参数与返回值写类型注解。`Q_INVOKABLE` 方法没有 FINAL，"可被遮蔽"告警对方法无法消除。
   - 宿主在创建 QML 引擎前用 `QmlInstance<T>::set()` 交出实例（偏好经 `exposePreferences()`）。未交出实例的单例被访问时返回空并报告严重告警，因此 QML 只访问其宿主提供的单例：独立终端提供偏好、`Shortcuts`、`MessageCenter`、`TerminalManager`、`UiIcons`、`WindowTransition`；画廊只提供偏好与 `UiIcons`。
3. 跨模块命令经 `WorkspaceController`。
4. 工作区模式由 `WorkspaceRuntime` 按依赖顺序构造全部服务，逆序销毁且先销毁 QML 引擎（见 [运行模式](runtime-modes.md)）。

## 2. 关键机制

- 窗口缩放、分屏拖动、DPI 变化经 `WindowTransitionCoordinator` 收敛为带代次的视口事务（见 [视口事务](../ui/layout/viewport-transitions.md)）。
- 工作区与独立终端一律 `QQuickWindow::QtTextRendering`。
- 子进程经 Job Object 与 UI 进程绑定，随主程序退出回收（见 [进程生命周期](process-lifecycle.md)）。

### 常量

- **契约常量**收敛到所属模块唯一的常量头 `src/<模块>/<模块>_constants.h`，各处引用名称，不重复字面量。契约常量包括：两个以上文件共用的值，以及持久化、协议或模块间约定的值（设置键与枚举值、数据库连接参数、任务操作名、会话选项键、工具 id）。
- 多层共用的常量放在用到它的最低层，上层不重新定义。
- 只在一个 `.cpp` 内使用的调优值（定时器间隔、超时、重试次数、缓冲上限）以 `k` 前缀命名，放在该文件顶部的匿名命名空间；函数体内不写裸数值，也不写函数内 `constexpr`。不言自明的值（0、1、-1、单位换算）除外。
- 字符串常量用 `inline constexpr QLatin1StringView`。
- 保留字面量的例外：属于某个类型的哨兵值与标志枚举随类型定义；`shell_extension_contract.h` 跨二进制共享，自成一份；迁移步骤冻结当时的字面量，不随常量改名；测试可直接写字面量钉住契约；含产品名的组合串（翻译路径、日志横幅、管道名）与交给 QML 的映射键（tile、`sessionsForProject`、远程主机与 shell 配置项）留在原处。
- QML 视觉值走 Theme token（见 [设计系统](../ui/design-system.md)）。

## 3. 本域改动完成标准

### 新增源码目录或静态库
- [ ] 有 `src/<域>/CMakeLists.txt`，只链接更下层；第 1–4 层不链接 `SpanApplication`。
- [ ] 本文 §1、[静态库分层](../workflow/build.md#静态库分层)、`docs/ownership.json` 已同步。
- [ ] 测试用 `spantui_add_library_test` 链接该库。

### 跨层移动类或改变依赖方向
- [ ] 调用方全部迁移，旧文件已删。
- [ ] 下层不 include `application/` 头文件。
- [ ] [状态所有权](state-ownership.md) 已更新。

### 修改组合根（`WorkspaceRuntime`）
- [ ] 成员按依赖顺序声明。
- [ ] 析构顺序仍为 `UiHealthMonitor` → QML 引擎 → 其余成员；新成员不会在引擎销毁后被 QML 访问。
- [ ] [运行模式](runtime-modes.md) 已同步。
