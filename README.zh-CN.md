# SpanTui

[English](README.md)

SpanTui 是一款基于 Qt 6 / QML 的 Windows 原生工作区与终端应用。它把项目浏览、代码与 Markdown 查看、Git 差异对比、SSH 远程主机和 ConPTY 终端集中在一个紧凑界面中，适合希望减少窗口切换、轻量高效的用户。

## 可以做什么

- 管理多个本地项目与文件目录树。
- 只读查看代码与普通文本，提供查找、跳转行、行号、Git hunk 色条与 Markdown 预览。
- 查看 Git 状态、提交历史图谱和差异比对（Diff）；写操作统一在终端内完成。
- 在应用内分屏运行多个 ConPTY 终端，保留本次运行期间的滚动历史，基于 libghostty-vt 支持 OSC 8 链接、Kitty 图像与键盘协议、同步输出等现代终端协议，并自带 ConPTY。
- SSH 远程主机：通过 SFTP 浏览远程文件、打开远程终端，使用系统自带的 OpenSSH 客户端。
- 自定义明暗主题、窗口材质、颜色、字体、光标和终端刷新率。
- 资源管理器文件夹右键菜单、独立终端模式，以及替换完整应用文件的独立更新器。
- 界面支持英文与简体中文。

## 两种使用方式

打开 SpanTui 会进入完整工作区。也可以从资源管理器的文件夹菜单打开指定项目。

独立终端模式只显示一个终端窗口，适合临时执行命令。它不会加入工作区的终端目录，关闭窗口时会结束该窗口启动的进程。

## 系统要求与安装

- Windows 10 1809（build 17763）及以上 x64；Windows 11 ARM64 经 x64 模拟运行。
- 从项目的 Releases 页面下载 `SpanTui-<版本>-Setup.exe` 安装程序或便携版压缩包。安装不需要管理员权限，唯一例外：Windows 11 上默认勾选的“资源管理器右键菜单”会请求一次批准，用于信任 SpanTui 的自签名发布者证书。拒绝批准时退回“显示更多选项”下的经典菜单项，取消勾选则不添加菜单项。

## 平台与数据

项目文件始终留在原位置；应用写的一切（设置、项目目录、工作区恢复信息、日志与缓存）都在一个数据目录里：安装版是 `%LOCALAPPDATA%\SpanTui`，便携版是 `SpanTui.exe` 旁的 `data` 文件夹（由同目录的 `portable.txt` 开启）。启动时加 `--data-dir <目录>` 可改用任意目录。终端输出不会在下次启动时恢复。

删除项目记录不会删除磁盘上的项目目录。文件树中的删除操作使用应用内部回收位置，并受可撤销范围限制。

## 从源码构建

需要：

- Qt 6.12.x MinGW 64-bit 套件，含 Qt Shader Tools 与 Qt SVG 模块
- MinGW 13.1 64-bit、CMake ≥ 3.22、Ninja（Qt 安装器的 Tools 中均可选装）
- 仅打包安装程序时需要 Inno Setup 6 与 Windows SDK

Qt 装在某盘的 `\Qt`（安装器默认 `C:\Qt`）或其 `qmake`/`cmake` 在 `PATH` 上时，构建脚本会自动发现，`dev.bat doctor` 列出找到的工具链；否则把 `.env.example` 复制为 `.env`，设置 `SPANTUI_QT_SDK_ROOT` 为 Qt 安装根目录（或逐项设置 `SPANTUI_QT_ROOT`、`SPANTUI_CMAKE`、`SPANTUI_MINGW_BIN`、`SPANTUI_NINJA_DIR`）；同名环境变量仍然优先。详见 [构建规范](docs/workflow/build.md)。

构建树与安装包放在仓库内已忽略的 `target\`；要放到别处，在 `.env` 里设置 `SPANTUI_BUILD_ROOT` 与 `SPANTUI_ARTIFACT_ROOT`。开发运行、测试与临时文件只写进已忽略的 `.dev\`，不碰你自己的 `%APPDATA%` 与 TEMP；`dev.bat clean` 删除它。

```cmd
dev.bat build                    :: 增量构建开发版 -> target\build\SpanTui-test.exe
start.bat                        :: 构建并启动开发版
dev.bat test -R terminal         :: 构建并运行名称匹配正则的测试
package.bat                      :: 在 target\release\<版本>\ 生成便携版与安装程序
dev.bat clean                    :: 删除 .dev\（开发数据、测试临时文件）
```

`src/terminal/vendor/conpty/x64/` 中捆绑的 ConPTY 可用 `tools/update-conpty.ps1` 从 NuGet 更新。

## 开发者

技术文档从 [docs/index.md](docs/index.md) 进入，整体结构见 [架构总览](docs/architecture/overview.md)。[AGENTS.md](AGENTS.md) 是 AI 编码助手在本仓库中遵循的约定。参与贡献请先读 [CONTRIBUTING.md](CONTRIBUTING.md)；安全问题按 [SECURITY.md](SECURITY.md) 私下报告。

## 许可证

SpanTui 以 [MIT License](LICENSE) 发布。随附的第三方组件保留各自的许可证：Qt（LGPL-3.0）、libghostty-vt 与 ConPTY（MIT），以及由 Geist、Figtree、Nerd Fonts 衍生的字体，详见 [第三方开源组件声明](docs/legal/third-party-notices.md)。
