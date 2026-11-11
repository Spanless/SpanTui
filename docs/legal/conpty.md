# ConPTY 来源与集成规范

随发布制品分发的 `conpty.dll` + `OpenConsole.exe`。运行时行为见 [捆绑 ConPTY](../services/terminal/conpty-lifecycle.md#捆绑-conpty)。

---

## 1. 上游来源与版本信息

- **NuGet 包**：[Microsoft.Windows.Console.ConPTY](https://www.nuget.org/packages/Microsoft.Windows.Console.ConPTY) `1.25.260930003`（包声明支持 Windows 10 1809 / 10.0.17763 及以上）
- **上游仓库**：[microsoft/terminal](https://github.com/microsoft/terminal)
- **开源许可证**：MIT License（正文位于 `src/terminal/vendor/conpty/LICENSE`）
- **签名**：两个文件均带有效的 Microsoft Corporation Authenticode 签名，文件版本 `1.25.2609.30003`

| 文件 | 包内路径 | SHA-256 |
| --- | --- | --- |
| `.nupkg` | — | `02B07B349AF66D801159BDF9E440D4A1CE78BB951F37FC8609731665AFDAE7EE` |
| `conpty.dll` | `runtimes/win-x64/native/conpty.dll` | `FEEEF341D891643C62D30B6B07800BC70F0BC148F44CB8C3BEE84AA557AE805A` |
| `OpenConsole.exe` | `build/native/runtimes/x64/OpenConsole.exe` | `3D66B23D0A71BB8EED2B77EDC8B9DF9BF54CE6C8FB74C863A30E760997F80586` |

---

## 2. 仓库位置与分发

- 二进制提交在 `src/terminal/vendor/conpty/x64/`，构建不联网。
- `src/terminal/CMakeLists.txt` 在配置时把两个文件复制到构建根目录，开发运行与终端测试使用与发布相同的控制台宿主。
- `tools/devtool/package-worker.ps1` 把两个文件放到 Portable 根目录 `SpanTui.exe` 旁；安装包复制整个 Portable 目录。
- `conpty.dll` 只在自身目录（其次 `<arch>\` 子目录）查找 `OpenConsole.exe`，找不到时静默改用系统 `conhost.exe`，所以两个文件必须同目录。

---

## 3. 更新流程

1. 运行 `tools\update-conpty.ps1 -Version <版本>`：下载到临时目录、要求两个文件均为有效的 Microsoft 签名、复制 x64 运行时并打印哈希。
2. 用打印出的版本与哈希更新本文第 1 节。
3. 收尾验证跑终端相关测试与 `dev.bat run --smoke`，确认启动日志含 `ConPTY: bundled`。
