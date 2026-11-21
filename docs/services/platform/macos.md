# macOS 平台

macOS（Apple Silicon，本机构建，不签名、不公证、无 CI）与 Windows 共用全部共享代码；平台差异只在各能力的实现文件里（拆分规则见 [架构总览](../../architecture/overview.md#平台实现)）。本文列出 macOS 实现与 Windows 行为的差异，Windows 侧见同级 `windows/` 文档。

## 1. 各能力的 macOS 实现

| 能力（中立头） | macOS 实现 | 与 Windows 的差异 |
|---|---|---|
| 终端进程 `terminal/api/platform_terminal.h` | `terminal/platform/macos/mac_pty_process.*`：`forkpty` | 见 §2 |
| Shell 目录 `terminal/api/shell_catalog.h` | `mac_shell_catalog.cpp`：`$SHELL` 优先，再按 `/etc/shells`（不含 `/bin/sh`） | id 为文件名（重名时用完整路径），一律以 `-l` 登录 shell 启动 |
| 窗口外框 `platform/window_chrome.h` | `macos/window_chrome.mm`：原生红绿灯，内容延伸到透明标题栏下，隐藏系统标题文字 | `systemCaptionButtons` 为真，QML 不画窗口按钮；`captionButtonsRect` 报告红绿灯在窗口内的矩形（启动、窗口缩放与退出全屏时重读；全屏时保留窗口态的值），标题带高度取其中线的两倍（系统标题栏高度），侧栏开关排在其右侧；`Main.qml` 的 `topPadding: 0` 让标题带画进标题栏的安全区；原生窗口在内容延伸前创建，QML 声明的矩形即窗框（先延伸会使窗框向上越出屏幕）；无深色窗框设置（随系统外观） |
| 进程包含 `platform/process_containment.h` | `macos/process_containment.cpp` | 没有 Job：`installApplicationContainment()` 不做事，`applicationJob*` 恒为假；受管与非受管启动都用 `QProcess::startDetached`。主程序崩溃时辅助进程不随之结束 |
| 打开方式 `platform/open_with_handlers.h` | `macos/open_with_handlers.mm`：`NSWorkspace` 按扩展名的 UTType 列出可打开的 `.app` | 跳过 `/System/` 下的应用；“在文件管理器中打开”为 Finder；文件夹经 `NSWorkspace openURLs` 交给目标应用 |
| 程序图标 `platform/program_icon_provider.h` | `macos/program_icon_provider.mm`：`NSWorkspace iconForFile` | 输入为 `.app` 或可执行文件路径 |
| 键盘惯例 `platform/keyboard_conventions.h` | `macos/keyboard_conventions.cpp`：`hasCommandKey()` 为真 | 快捷键默认用 ⌘ 组合（终端 ⌘C/⌘V/⌘F 等），窗口命令进原生菜单栏 `MacMenuBar.qml`（文件/编辑/显示/窗口；应用菜单的关于、设置、隐藏、退出由 Qt 按 role 并入），Windows 用 Ctrl+Shift 组合与窗口级 `Shortcut`（见 [设置 §快捷键](../application/settings-appearance.md#快捷键)）；物理 Control 组合照常进 PTY |
| 单实例锁 `platform/instance_lock.h` | `macos/instance_lock.cpp`：`<端点地址>.lock` 上的 `flock` | 持锁者在监听前删掉残留的本地套接字文件 |
| 本地端点 `platform/local_endpoint.h` | `macos/local_endpoint.cpp`：`confstr(_CS_DARWIN_USER_TEMP_DIR)` 下的 Unix 域套接字 | 不随 `$TMPDIR` 变化：同一用户的进程总能找到彼此，路径也在 `sun_path` 的 104 字节内（端点名须短于约 50 字符）；单实例与 SSH 提示桥都经它取地址 |
| 图形适配器 `platform/graphics_adapter.h` | `macos/graphics_adapter.mm`：Metal 设备的 `MTLDevice.name` | 只有 Metal；没有软件适配器 |
| 减少动态效果 `platform/accessibility_preferences.h` | `accessibilityDisplayShouldReduceMotion` | — |
| 凭据保护 `platform/credential_protection.h` | `macos/credential_protection.cpp` | 见 §3 |
| 原生对话框 `platform/native_dialog.h` | `macos/native_dialog.cpp`：`CFUserNotificationDisplayAlert` | 按钮文案经 `NativeDialog` 上下文翻译 |
| 崩溃处理 `platform/crash_handler.h` | `macos/crash_handler.cpp`：致命信号处理器，`backtrace` | 报告格式相同，栈标题为 `Stack (backtrace):`；没有 minidump |
| 目录监视 `platform/directory_change_watcher.h` | `macos/directory_change_watcher.cpp`：FSEvents | 事件丢失（`MustScanSubDirs`、内核或用户端丢弃）按溢出处理，触发整树重扫 |
| 资源采样 `monitoring/platform_resource_sampler.h` | `monitoring/macos/`：`proc_pidinfo`、`task_info` | 内存取 `phys_footprint`；没有按进程的 GPU 计数器，`GpuCounterStatus::Unavailable` |
| 脚本运行 `platform/script_launch.h` | `macos/script_launch.cpp` | `.sh`/`.command`/`.bash`/`.zsh`，在登录 shell 里运行后保留交互式 shell（见 [脚本运行](../terminal/standalone-script-launch.md#2-文件树运行脚本)） |
| 外部工具位置 `core/external_tool_locations.h` | `core/macos/`：`/etc/paths`、`/etc/paths.d`，Homebrew 目录，`/usr/bin` | 可执行文件无扩展名 |
| 数据目录 `core/platform_folders.h` | `~/Library/Application Support` | 无旧数据根迁移 |
| SSH 提示桥 `remote/ssh_askpass_platform.h` | `remote/macos/`：Unix 域套接字，`proc_pidinfo` 查父进程 | — |

## 2. 终端进程

- **启动**：命令行按 `QProcess::splitCommand` 拆分（与 Windows 的 MSVC 规则不同）；裸程序名按 PATH 查找；工作目录不存在时启动失败，为空时取主目录。
- **子进程**（`fork` 之后只调用异步信号安全函数）：恢复信号掩码与全部信号处置，设置 `IUTF8`，切换目录后 `execve`；失败的 `errno` 经 `CLOEXEC` 管道报告给父进程，`start()` 返回系统错误文本。
- **环境**：继承进程环境，去掉其他终端的会话变量（`TERM_SESSION_ID`、`ITERM_SESSION_ID`、`LC_TERMINAL*`），设置 `TERM=xterm-256color`、`COLORTERM=truecolor`、`TERM_PROGRAM`、`COLORFGBG`；三个区域变量都没有时补 `LANG=<系统区域>.UTF-8`；最后叠加调用方的变量。
- **I/O**：主端非阻塞；I/O 线程以 `select()` 等待主端与唤醒管道（macOS 的 `poll()` 不支持设备文件），写入积压上限 1 MiB；等待线程阻塞在 `waitpid`。退出码在最后一段输出之后回调；被信号终止时为 `128 + 信号`。`select()` 非 `EINTR` 失败时会话按挂断结束：停止写入、挂断子进程，宽限后 `SIGKILL`，仍照常回调退出。
- **中断**：丢弃积压写入并写入 `^C`，由行规程向前台作业发 `SIGINT`。
- **停止**：关闭主端前先记下前台进程组（`tcgetpgrp`），向 shell 的进程组与前台作业的进程组发 `SIGHUP`、关闭主端，250 ms 后 shell 仍在则 `SIGKILL`，并对与 shell 不同的前台作业进程组同样 `SIGKILL`——忽略 `SIGHUP` 的前台作业（`set -m` 下的作业不在 shell 的进程组）不会残留；`stop()` 之后不再回调退出。
- **按键**：Qt 在 macOS 上把 ⌘ 报为 Control、Control 报为 Meta；终端在 `translateKey` 换回物理修饰键（未设置 `AA_MacDontSwapCtrlAndMeta` 时），⌘ 组合键留给应用快捷键，未绑定终端动作的 ⌘ 组合键不经 ShortcutOverride 认领，菜单栏的键等价项（⌘Q 退出、⌘H 隐藏）在终端获焦时照常生效，Option 组合出字符时作为文本输入。

测试：`mac_pty_process_test`（回显、初始尺寸与调整、工作目录、环境、退出码与信号退出、中断、停止、停止时结束忽略挂断的前台作业、启动失败）。

## 3. 凭据保护的局限

Windows 用 DPAPI 绑定当前用户。macOS 版本未签名，钥匙串条目会随每次重新构建（签名身份变化）反复要求授权，因此改用数据根下的密钥文件 `ProductPaths::credentialKeyPath()`（仅当前用户可读写）：AES-256-CBC 加密，HMAC-SHA256 校验（CommonCrypto）。只有密钥文件不存在时才生成；文件存在但不可读或长度不对时保留原文件、加密与解密都失败，不覆盖——覆盖会让已保存的密文全部无法解密。测试：`credential_protection_test`（仅 macOS 注册；Windows 的 DPAPI 由 `remote_host_registry_test` 覆盖）。

**这比 DPAPI 弱**：能读到数据根的任何进程或备份都能同时拿到密钥与密文。只在本机自用场景下接受；签名后应改为钥匙串。

## 4. 已知局限

- 应用内更新只能检查、不能安装：安装镜像以程序旁的 `unins*.exe` 识别，macOS 构建总是开发镜像（`canInstall` 为假）。
- 资源管理器右键菜单（Shell 扩展）无对应实现。
- 远程主机的终端命令行按 Windows 规则拼接（`remote/ssh_command.h` 的 `windowsCommandLine`），macOS 终端按 `QProcess::splitCommand` 拆分；两者只在参数含双引号（或其前的反斜杠）时不一致，主机名、端口、用户名与含空格的密钥路径不受影响。
- 界面卡顿报告（`diagnostics/hang/hang-latest.log`）在两个平台上都不含 GUI 线程的调用栈。
- 鼠标事件的修饰键未换回物理键。
- Option 组合出字符（如 Option+x 得到 `≈`）时作为文本发送，不作为 Meta 前缀；只有未组合出字符的 Option 组合键作为 Alt 编码（`GhosttyEngine::encodeKey` 打开 libghostty 的 `macos_option_as_alt`）。
- Windows 专属测试（ConPTY、Job、Shell 扩展、IME、命名管道、文件 ACL 及其辅助子进程）只在 Windows 注册；跨平台测试里启动安装程序的两个更新用例在 macOS 上跳过（见 [测试设计规范](../../workflow/testing.md)）。

## 5. 本域改动完成标准

- [ ] 新的平台能力是一个中立头加 `windows/`、`macos/` 两份实现，由所在模块的 `CMakeLists.txt` 按平台选择；共享代码不写 `#ifdef Q_OS_*`。
- [ ] 改动 Windows 实现的同时检查 macOS 实现（反之亦然），行为差异写进本文 §1。
- [ ] macOS 上增量构建并运行受影响测试；Windows 侧由 CI 编译。
