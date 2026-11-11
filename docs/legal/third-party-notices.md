# 第三方开源组件声明 (Third-Party Notices)

SpanTui 本身以 MIT License 发布（见仓库根目录 `LICENSE`，发行包内 `licenses/SpanTui-LICENSE.txt`）。下列第三方组件静态嵌入可执行程序，或随发布制品（Portable / Setup）以独立文件分发。

本文件在发行包中名为 `THIRD_PARTY_NOTICES.md`；所引用的许可证正文都随包放在 `licenses/` 目录（由 `tools/devtool/package-worker.ps1` 复制：`docs/legal/licenses/` 与字体许可证整目录复制，其余按固定文件名复制，缺失即打包失败）。下文"许可证正文"一栏先写发行包内路径，括号内为源码仓库路径。

---

## 1. libghostty-vt

- **组件说明**：Ghostty 的终端模拟库（转义序列解析、屏幕与滚动历史、按键与鼠标编码），以预编译静态库链接，无源码补丁。版权归 Mitchell Hashimoto 与 Ghostty contributors。
- **资源位置**：`src/terminal/vendor/libghostty-vt/`
- **开源许可证**：MIT License
- **许可证正文**：`licenses/libghostty-vt-LICENSE.txt`（`src/terminal/vendor/libghostty-vt/LICENSE`）
- **上游地址**：[ghostty-org/ghostty](https://github.com/ghostty-org/ghostty)
- **专题说明**：参见 [libghostty-vt 来源与集成规范](libghostty-vt.md)

---

## 2. ConPTY（conpty.dll / OpenConsole.exe）

- **组件说明**：Windows 伪控制台与控制台宿主，取自微软 NuGet 包 `Microsoft.Windows.Console.ConPTY`，随 Portable / Setup 放在 `SpanTui.exe` 旁。
- **资源位置**：`src/terminal/vendor/conpty/x64/`
- **开源许可证**：MIT License
- **许可证正文**：`licenses/ConPTY-LICENSE.txt`（`src/terminal/vendor/conpty/LICENSE`）
- **上游地址**：[microsoft/terminal](https://github.com/microsoft/terminal)
- **专题说明**：参见 [ConPTY 来源与集成规范](conpty.md)

---

## 3. Qt 6 框架

- **组件说明**：GUI、网络、QML、Quick 控件、SQL（仅 SQLite 驱动）与 SVG 渲染。随包模块来自 qtbase、qtdeclarative、qtsvg、qtimageformats 与 qttranslations。
- **版本**：以发行包 `licenses/Qt/*.spdx` 中记录的版本为准（当前构建使用 Qt 6.12.0）。
- **分发方式**：打包流水线通过 `windeployqt` 部署 Qt 动态链接库（DLL）及 QML 插件，SpanTui 以动态链接方式使用 Qt；用户可以用自行构建或修改的同版本兼容 Qt DLL 替换包内文件。唯一静态链接的 Qt 部分是 `EntryPointPrivate`（BSD-3-Clause）。
- **开源许可证**：GNU LGPL v3（`licenses/LGPL-3.0.txt`；LGPL v3 以 GPL v3 为基础，正文见 `licenses/GPL-3.0.txt`）。仓库内副本位于 `docs/legal/licenses/`。
- **不随包分发**：开源版仅以 GPL v3 提供的模块（如 Qt Virtual Keyboard）不得进入发行包；`package-worker.ps1` 在部署时排除并在检测到时打包失败。
- **Qt 内置第三方组件**：Qt 自带的 zlib、libpng、libjpeg、libwebp、libtiff、HarfBuzz、FreeType、PCRE2、md4c、SQLite 等组件，名称、版本、许可证与版权声明见随包的 Qt SBOM（`licenses/Qt/qtbase-*.spdx` 等，取自 Qt 安装目录 `sbom/`）。
- **源码获取**：Qt 源码可从 [download.qt.io](https://download.qt.io/official_releases/qt/) 按对应版本获取。
- **上游地址**：[https://www.qt.io/licensing/](https://www.qt.io/licensing/)

---

## 4. MinGW-w64 运行库

- **组件说明**：`libgcc_s_seh-1.dll`、`libstdc++-6.dll`、`libwinpthread-1.dll`，由 `windeployqt --compiler-runtime` 从构建所用的 MinGW 工具链复制。
- **开源许可证**：
  - `libgcc_s_seh-1.dll`、`libstdc++-6.dll`：GPL v3 附 GCC Runtime Library Exception 3.1（`licenses/GPL-3.0.txt`、`licenses/GCC-RUNTIME-LIBRARY-EXCEPTION-3.1.txt`）。该例外允许以任意许可证分发链接这些运行库的程序。
  - `libwinpthread-1.dll`：MIT 风格许可（`licenses/winpthreads-COPYING.txt`）。
- **上游地址**：[GCC](https://gcc.gnu.org/)、[mingw-w64](https://www.mingw-w64.org/)

---

## 5. D3Dcompiler_47.dll

- **组件说明**：Direct3D 着色器编译器，由 `windeployqt` 随 Qt 运行库部署，供 Qt Quick 的 D3D 渲染后端使用。
- **许可**：Microsoft 可再分发组件，按 Windows SDK 的可再分发条款随应用分发。

---

## 6. Span Mono（Geist Mono 定制版）

- **组件说明**：SpanTui 内置的等宽字体，由 Geist Mono 经 `tools/fonts/make_span_fonts.py` 改名、固化样式集，并自绘 Powerline 分隔符与替换字符 `U+FFFD` 而成；图标字体 Span Symbols 的度量与 `.notdef` 也取自 Geist Mono；以新的家族名发布，属 OFL 所称的修改版本（Modified Version）。字体嵌入可执行程序资源。
- **资源位置**：`assets/fonts/SpanMono-*.ttf`、`assets/fonts/SpanSymbols-Regular.ttf`
- **开源许可证**：SIL Open Font License 1.1
- **许可证正文**：`licenses/SpanFonts-LICENSE-Geist.txt`（`assets/fonts/SpanFonts-LICENSE-Geist.txt`）
- **上游地址**：[vercel/geist-font](https://github.com/vercel/geist-font)

---

## 7. Span Sans（Figtree 定制版）

- **组件说明**：SpanTui 内置的界面字体，由 Figtree 经 `tools/fonts/make_span_fonts.py` 改名、固化样式集而成；以新的家族名发布，属 OFL 所称的修改版本（Modified Version）。字体嵌入可执行程序资源。
- **资源位置**：`assets/fonts/SpanSans-*.ttf`
- **开源许可证**：SIL Open Font License 1.1
- **许可证正文**：`licenses/SpanFonts-LICENSE-Figtree.txt`（`assets/fonts/SpanFonts-LICENSE-Figtree.txt`）
- **上游地址**：[erikdkennedy/figtree](https://github.com/erikdkennedy/figtree)

---

## 8. Nerd Fonts Symbols

- **组件说明**：Span Symbols 中的图标字形，取自 Nerd Fonts `NerdFontsSymbolsOnly` 发布包；`U+E0B0`–`U+E0BF` 的 Powerline 分隔符为 SpanTui 自绘，不取自该包。
- **资源位置**：`assets/fonts/SpanSymbols-Regular.ttf`
- **开源许可证**：Nerd Fonts 项目为 MIT License（`licenses/SpanFonts-LICENSE-NerdFonts.txt`）；各图标集保留其上游许可证与署名：

  | 图标集 | 版权方 | 许可证 |
  |---|---|---|
  | Font Awesome | Fonticons, Inc. | CC BY 4.0（[许可证](https://creativecommons.org/licenses/by/4.0/)） |
  | Codicons | Microsoft Corporation | CC BY 4.0（[许可证](https://creativecommons.org/licenses/by/4.0/)） |
  | Material Design Icons | Pictogrammers | Apache License 2.0（`licenses/Apache-2.0.txt`） |
  | Octicons | GitHub, Inc. | MIT |
  | Devicons | Theodore Vorillas | MIT |
  | Powerline Extra Symbols | Ryan L McIntyre | MIT |
  | Seti UI | Jesse Weed | MIT |
  | Weather Icons、Pomicons | Erik Flowers、Gabriele Lana | SIL OFL 1.1 |
  | Font Logos | Lukas W | Unlicense |

  以上字形均未经修改，仅重新打包进 Span Symbols。完整清单以 Nerd Fonts 仓库 `license-audit.md` 为准。
- **上游地址**：[ryanoasis/nerd-fonts](https://github.com/ryanoasis/nerd-fonts)

---

## 9. 应用图标与着色器

`assets/icons/`、`assets/brand/` 中的 SVG/ICO 图标与 `shaders/` 中的着色器为 SpanTui 自绘或自写，随项目以 MIT License 发布。唯一的第三方来源：终端文字的距离场着色（`shaders/terminal_cell.frag` 的灰度阈值、`shaders/terminal_cell_subpixel.frag` 的子像素采样与合成）及其阈值函数（`src/terminal/render/terminal_cell_material.cpp` 的 `TerminalFieldText`）按 Qt Quick 距离场文字的着色器（`distancefieldtext.frag`、`hiqsubpixeldistancefieldtext.frag`）与 `QT_DF_*` 默认参数改写，版权归 The Qt Company Ltd.，许可证同 §3（LGPL-3.0）。
