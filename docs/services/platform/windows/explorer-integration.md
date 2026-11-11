# 资源管理器右键菜单

## 1. Shell 扩展

`src/platform/windows/shell_extension.cpp` 编译为 `SpanTui-shell-extension.dll`（`IExplorerCommand` COM），只依赖 `ole32`、`shell32`、`uuid`，不加载 Qt，不读数据库、不建窗口，只取所选文件夹并启动 `SpanTui.exe`。

顶级菜单「SpanTui」（`ECF_HASSUBCOMMANDS`）：

- 在工作区中打开 → `SpanTui.exe --project "<文件夹>"`
- 在此处打开独立终端 → `SpanTui.exe --terminal --cwd "<文件夹>"`

文案按系统界面语言选中英文。选中文件时不启动。CLSID 等契约在 `shell_extension_contract.h`。

## 2. 安装注册

安装程序可选任务 `contextmenu`（默认勾选；静默安装用 `/TASKS`、`/MERGETASKS`）。逻辑在 `installer/SpanTui.iss` 的 `[Code]`：

- **Windows 11**：经稀疏包身份注册 COM，显示在一级菜单。自签名发布者需一次 UAC 把证书写入 `LocalMachine\TrustedPeople`。
- **降级**：拒绝 UAC、侧载被禁或注册失败时撤销已写信任，改写经典菜单并提示一次（静默安装不提示）。右键菜单失败不让安装失败。
- **Windows 10**：写 `HKCU\Software\Classes\Directory\shell` 与 `Directory\Background\shell`，无需提权。
- 实际结果（`modern`/`classic`/`none`）记在 `HKCU\Software\Span\SpanTui\Setup` 的 `ShellMenu`，升级与卸载据此只撤销真实存在的注册（只有 `modern` 撤销时提权）。无记录的旧安装按 Windows 11 = `modern`、其余 = `classic` 推断。
- 取消勾选的升级会移除上一版注册。

便携版不注册菜单、不写注册表。安装程序要求 Windows 10 1809（`MinVersion=10.0.17763`）：首个提供 ConPTY 且 Qt 6 支持的版本。

测试：`shell_extension_contract_test`（扩展副本旁放 `argv_echo_child` 冒充 `SpanTui.exe`，核对两条子命令的真实参数）。
