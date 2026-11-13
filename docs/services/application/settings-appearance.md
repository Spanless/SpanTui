# 设置与外观

全局设置存 SQLite `settings` 表，按域分三个偏好对象，由 `Preferences`（`src/application/preferences.*`）组装，宿主经 `exposePreferences()`（`src/application/runtime/qml_types.h`）交给声明式 QML 单例（URI `SpanTui`）：

| 对象 | QML 单例 | 拥有 |
|---|---|---|
| `AppearanceSettings` | `Appearance` | 语言、主题、系统显示事实（深色、减弱动效、滚轮行数）、五条字体通道、查看器行距 |
| `TerminalSettings` | `TerminalSettings` | 终端配色方案与明暗槽位、光标、帧率、关闭瓦片默认、Shell 配置 |
| `WorkspaceSettings` | `WorkspaceSettings` | 脚本运行模式、渲染方式、资源采样间隔、快捷键覆盖、外部工具路径、工作树文件夹 |

另有无状态单例 `Clipboard`（`copyText`）。三者只经 `SettingsStore`（`src/application/settings_store.*`）读写：setter 先改内存值并暂存，250ms 防抖后在一个事务内写入全部暂存键；读取时暂存值优先；写入失败的键保持暂存并经 `errorOccurred` 报告。只读模式（独立终端）从不写库，库不存在时全部取默认值。未 `open()` 的存储（组件画廊）同样只提供默认值。`WorkspaceSettings::setRenderMode` 立即 `flush()`——图形启动中崩溃的进程也要留下选择。

## 1. 通知信号

| 对象 | 信号 | 范围 |
|---|---|---|
| `Appearance` | `appearanceChanged` | 语言、主题、非终端字体通道、查看器行距 |
| | `effectiveDarkChanged` | 实际明暗翻转（主题或系统） |
| | `terminalFontChanged` | 终端字体通道 |
| | `fontRevisionChanged` | 任一字体变化；QML 字号函数靠它重算 |
| | `reduceMotionChanged`、`wheelScrollLinesChanged` | 系统显示事实 |
| `TerminalSettings` | `appearanceChanged` | 配色、光标、帧率；实际明暗翻转时也发 |
| | `colorSchemesChanged` | 自定义配色集合 |
| | `closePreferenceChanged` | 关闭瓦片默认 |
| | `shellProfilesChanged` | Shell 配置 |
| `WorkspaceSettings` | `scriptRunModeChanged`、`renderModeChanged`、`performanceMonitorIntervalChanged` | 各自一项 |
| | `shortcutOverridesChanged` | 快捷键（`ShortcutRegistry` 转发为 `bindingsChanged` 并递增 `revision`） |
| | `toolPathOverridesChanged` | 外部工具路径 |
| | `worktreeRootChanged` | 工作树文件夹 |

新设置优先挂到所属对象的已有信号；只有会让无关绑定误触发时才新建。

## 2. 设置项

| 键 | 取值 | 说明 |
|---|---|---|
| `language` | `zh_CN`（默认）、`en` | 下拉框中每种语言用自身文字命名，不翻译 |
| `themeMode` | `system`、`light`、`dark` | |
| `<通道>PrimaryFontFamily`、`<通道>FallbackFontFamily`、`<通道>FontSize`、`<通道>FontWeight` | `ui` 14（10–18）、`uiAux` 12（8–16）、`formHint` 10（8–16）、`editor` 13（10–22）、`terminal` 13（10–22） | 每通道主字体 + 回退字体；只有终端通道对非等宽字体给警示 |
| `editorLineHeight` | 1.0、1.25、1.5、2.0 | 查看器行距 |
| `renderMode` | `auto`、`software`；其他按 `auto` | 每次启动读一次 |
| `terminal.cursorStyle` | `block`、`bar`、`underline` | DECSCUSR 可覆盖，`CSI 0 SP q` 交还 |
| `terminal.refreshRate` | 30–360 Hz 档位 | 终端最高发布帧率 |
| `terminal.shellProfiles` | JSON：`{default, arguments: {id: 参数}, custom: [{id, name, executable, arguments}]}` | 非对象丢弃 |
| `terminal.colorSchemes`、`terminal.colorSchemeLight`/`Dark` | JSON | 自定义配色，见 §4 |
| `workspace.scriptRunMode` | `projectSession`、`independentWindow` | 见 [脚本运行](../terminal/standalone-script-launch.md) |
| `workspace.terminateTerminalOnTileClose` | bool | 关闭终端瓦片时的默认勾选 |
| `monitor.intervalSeconds` | 1–60 | 资源采样间隔 |
| `shortcuts.bindings` | JSON：动作 id → 按键序列 | 只存对默认值的覆盖 |
| `tools.paths` | JSON：`git`/`ssh`/`sftp`/`tailscale` → 绝对路径 | 空值与非字符串丢弃 |
| `git.worktreeRoot` | 绝对文件夹路径；空 = 未设置 | 新工作树的默认存放处（`<它>\<主项目名>\<名称>`）。空时首次新建工作树先询问；非绝对路径按空处理。设置页“环境 > Git”可改（见 [项目与会话](projects-sessions.md#新建工作树)） |

同表另有窗口几何（`window.x`/`y`/`width`/`height`/`maximized`）、`workspace.layoutJson`（见 [布局模型](../workspace/layout-tiles.md)）、`sessions.activeByProject`。

- 载入与 QML 写入都做范围与枚举归一，非法值回退默认，不中断启动。
- 只识别上表的键；旧版残留键（`darkTheme`、`*FontFamilies` 等）不读不写。
- `shortcuts.bindings` 未知 id 与无法解析的序列被忽略但原样写回，新版本写入的条目在旧版本中不丢失；空串表示解绑。`terminal.shellProfiles` 由 `ShellProfileRegistry` 校验，未知 id 回退默认。

### 新增设置项

1. 在所属域对象（`appearance_settings.h`/`terminal_settings.h`/`workspace_settings.h`）加 `Q_PROPERTY`（READ/WRITE/NOTIFY）。
2. `load()` 经 `SettingsStore::value()` 读取并校验，非法值回退默认，不得中断启动；setter 经 `SettingsStore::setValue()` 暂存，只写变化的键。
3. 键名 `domain.camelCase`：外观类（语言、主题、字体、行距、渲染模式）无前缀；其余按域加前缀（`terminal.`、`workspace.`、`monitor.`、`shortcuts.`、`tools.`、`window.`、`sessions.`）。已写入的键改名经 `WorkspaceDatabaseMigration`。
4. 同步本表、`settings_test`（默认值、读写、非法值回退）与 `.ts` 译文。

## 3. 设置页

### Shell

`ShellProfileRegistry`（QML 单例 `Shells`）：

- 检测顺序（`terminal::detectShells`）：`pwsh`、`powershell`、`cmd`、`git-bash`（当前 Git 安装目录的 `bin\bash.exe --login -i`）、每个 WSL 发行版（`wsl:<名称>`，读 `HKCU\…\Lxss`，跳过 `docker-desktop*`）。PowerShell 默认参数 `-NoLogo`。
- 名称随界面语言（`retranslate()`）；图标取程序自身图标（Git Bash 取 `git-bash.exe`）。
- 默认 Shell 只能选可启动的；未选或已失效时取检测顺序首项。
- 检测到的 Shell 只保存与默认不同的参数。自定义 Shell 程序不存在时提示「未找到」，不能设为默认、不进菜单。
- 项目菜单「新建终端」用默认 Shell，其余在「更多终端」子菜单。指定 Shell 的会话把 id 存进 `sessions.shell_profile`（`''` 跟随默认）。命令行在创建终端时确定，改参数只影响之后新建的终端。

### 快捷键

`ShortcutRegistry` 目录按「终端 / 文件查看器 / Git 历史」分组，另有只读的「固定按键」（终端 Ctrl+C、查找栏 Enter/Shift+Enter/Esc）。

| 动作 id | 默认 | 适用条件 |
|---|---|---|
| `terminal.find` | Ctrl+Shift+F | |
| `terminal.copy` | Ctrl+Shift+C | 有选区 |
| `terminal.paste` | Ctrl+Shift+V | |
| `terminal.previousPrompt` / `nextPrompt` | Ctrl+Shift+Up / Down | 有 OSC 133 标记 |
| `editor.find` | Ctrl+F | |
| `editor.goToLine` | Ctrl+G | |
| `editor.copyLine` | Ctrl+C | |
| `git.copyHash` | Ctrl+C | |

- **录制**：`ShortcutRecorder` 获焦即录制，经 `Keys.onShortcutOverride` 认领全部按键；Esc 取消、Backspace/Delete 解绑、无修饰 Tab 保留焦点移动。
- **校验**：`setBinding` 只接受单个组合，须带 Ctrl/Alt 或为 F1–F24（裸字母会抢走 shell 输入）。同一组合可绑多个动作，`catalog()` 的 `sharedWith` 列出作用域相同（或任一为 global）的共用者；按下时执行全部适用的动作，都不适用才按普通按键处理。
- **消费**：QML 用 `Shortcuts.matches(id, key, modifiers)`；终端经 `TerminalView.shortcutBindings`（见 [输入协议 §1.4](../terminal/input-protocols.md#14-视图层约定)）。标题与说明文案在 QML，C++ 只返回 id 与错误码。

### 环境页

`ToolRegistry`（QML 单例 `Tools`）列出 Git、ssh、sftp、tailscale（可选）。

- 路径框显示正在使用的路径（指定优先，否则检测结果，都没有则空）。输入或选择后写 `tools.paths` 并重新检测；指定路径须是绝对路径的可执行文件，否则状态为「指定路径无效」且工具不可用——不回退到自动检测。撤销按钮清除指定。
- 「重新检测」重读 PATH（含注册表持久值）并重跑 `git --version`、`ssh -V`、`tailscale version`（5 秒超时）。
- 启动时找到工具即视为可用，版本探测失败再收回，避免界面闪现。
- 影响：Git 不可用隐藏右栏更改/历史页并停止全部 git 调用；ssh 不可用隐藏侧栏「远程」节；tailscale 不可用时远程页设备列表换成安装提示。
- **渲染**：硬件/软件写 `renderMode`（`GraphicsModeService`，QML 单例 `Graphics`），下次启动生效。提示行给出需重启、本次 `--safe-graphics`、上次失败已自动切换；「当前渲染」显示首帧后读取的适配器与图形 API（机制见 [图形启动](startup-single-instance.md#图形启动与软件渲染)）。

### 远程页

- **SSH 提示桥**（ssh 可用时）：显示 askpass 管道名（`RemoteHosts.promptBridgePipe`）与运行状态（`promptBridgeAvailable`/`promptBridgeError`）。
- **Tailscale 设备**：页面显示与点刷新时读一次（`TailscaleDevices`，不轮询）。「仅显示可 SSH 的设备」默认开、不持久化。每行显示机器名、SSH 状态、Tailscale 地址，按钮为「添加…」（预填主机对话框）或「连接」（地址已是保存主机时）；开始会话即关闭设置。判定见 [SSH 远程 §7](../remote/ssh-remote.md#7-tailscale-设备)。

### 开发者页

「从本地构建更新」（`AppUpdate.localSourceEnabled`）与本地构建文件夹（`AppUpdate.localSourcePath`，`dev package` 各版本文件夹的上级）存 `settings.ini` 的 `[update]` 组，不进 SQLite。行为见 [应用更新 §1](../update/updater.md#1-更新源检测与确认流程)。

## 4. 配色方案

自定义方案含 ANSI 16 色、前景、背景、选区、界面色 `uiBackground`/`uiPlate`/`uiAccent` 与交互状态层 `uiHover`/`uiSelection`/`uiPressed`（`#AARRGGBB`，语义见 [交互状态层](../../ui/design-system.md#交互状态)），存 `terminal.colorSchemes`。缺界面键的方案读取时由 `completeSchemeUi()` 补齐。

## 5. 文本渲染与内置字体

工作区与独立终端启动时固定 `QQuickWindow::QtTextRendering`。

### 内置字体

`main.cpp` 注册 `:/fonts/` 下全部 `.ttf`：

| 家族 | 字重 | 用途 | 来源 |
|---|---|---|---|
| Span Mono | 100–900，各带斜体 | `editor`、`terminal` 默认；`TerminalFontSet` 兜底 | Geist Mono，固化 `ss03`、`ss08`；自绘 Powerline 与 `U+FFFD` |
| Span Sans | 300–900，各带斜体 | `ui`、`uiAux`、`formHint` 默认 | Figtree，固化 `ss01`、`ss02` |
| Span Symbols | Regular | 仅终端回退链的图标字体，不出现在选择器 | Nerd Fonts Symbols + 自绘 Powerline，按 Span Mono 格度量 |

- "固化"即把样式集替换字形写进 cmap，不依赖 OpenType 特性；无编程连字。
- 同家族所有字重共用家族名，选择器只出现 Span Mono / Span Sans。
- 图标独立成 Span Symbols，终端主字体换掉后仍能显示 Nerd 图标。
- 不含 CJK（由回退字体承担）；框线、方块、盲文由终端自绘（见 [渲染 §2](../terminal/rendering-scrollback.md#2-特殊字形几何)）。
- 度量沿用 Geist Mono typo（`USE_TYPO_METRICS`）：宽 0.6em，升 1005、降 295，终端行高 1.3em。
- Powerline `U+E0B0`–`U+E0B7` 一律绘成半圆，`U+E0B8`–`U+E0BF` 为对角三角与斜线；按单元格精确绘制，平边略出血防细缝。
- `U+FFFD` 自绘为占满格宽的实心菱形挖空 `?`：终端把无矢量字体收录的码位换成它，Geist Mono 本身不含此字。
- 默认值只在设置为空时采用，升级不改已有设置。
- 重新生成：`tools/fonts/build-fonts.ps1`（Python + fontTools，固定上游版本并校验 SHA-256，工作目录 `.dev/tmp/fontbuild/`），产物写回 `assets/fonts/`。正常构建不需要。

## 6. 代码与测试

- 设置界面：`qml/Settings.qml`、`qml/components/business/settings/`
- `src/application/{preferences, settings_store, appearance_settings, terminal_settings, workspace_settings, clipboard}.*`
- `src/application/{shortcut_registry, tool_registry, graphics_mode_service, shell_profile_registry}.*`、`src/core/external_tools.*`、`src/remote/tailscale_devices.*`、`src/terminal/platform/windows/win_shell_catalog.cpp`
- 测试：`settings_test`、`shortcut_registry_test`、`shell_profile_registry_test`
