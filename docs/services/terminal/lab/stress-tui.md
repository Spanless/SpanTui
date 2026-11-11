# 终端压测 TUI

`tools/terminal_stress_tui/`：故意写得很差的 TUI（卡顿、分片写入、巨量输出、协议误用），用于复现撕裂与卡顿；`protocol` 场景则规范地逐项覆盖 VT 协议，用于核对渲染。

## 1. 原则

- 独立 Qt Core 工程，不进主构建与安装包。
- **撕裂可见**：同一帧每行带相同的帧戳（`F000123`）与同色背景带；屏上出现两种即为撕裂。`omp-transcript` 把帧戳放在输入框边框与状态行，两处不一致即差分绘制被切开。
- **可重放**：同 `--seed`、同尺寸生成相同字节流（`chaos` 的切换时刻除外）。卡顿靠阻塞事件循环，在两次 `WriteFile` 之间停顿。
- 不读输入、不改用户数据、不发 BEL 或 OSC 52；`Ctrl+C` 退出时复位全部模式。

## 2. 场景

| 场景 | 模拟 | 压力点 |
|---|---|---|
| `omp-transcript` | 主屏聊天记录 | 从首个变化行差分重写；输入框与状态行每帧变；视口上方变化或宽度变化时 `CSI 2J CSI 3J` 后整段重印（默认上限 4 MiB） |
| `fullscreen-churn` | 备用屏仪表盘 | 每帧整屏重绘：方块、盲文、CJK、真彩色；默认不带 2026 |
| `log-flood` | 日志洪流 | 每次 50–600 行，含长换行行与 `\r` 进度条 |
| `stall-burst` | 等待模型的 CLI | 长时间无输出后一次写 256 KiB–1 MiB |
| `broken-protocol` | 写坏协议的 TUI | 丢 `?2026l`、重复 set/reset、ESC 与 CSI 拆开、UTF-8 拆开、越界参数、未结束 OSC、2026 中途停顿 |
| `chaos` | 以上全部 | 每 3–10 秒随机切换，切换时复位 |
| `protocol` | 规范应用 | 备用屏逐页覆盖协议，每页约 4 秒，每帧一次完整写入；不参与 `chaos` |

`--jank`（默认 30%）为一帧被拆成随机分片（每片 ≤ `--fragment-max` 字节）并中途停顿至多 `--stall-max-ms` 的概率。`stall-burst`、`broken-protocol`、`protocol` 自定写入边界，不再拆分。

## 3. 协议覆盖（`protocol` 场景）

`--protocol-stage <名称>` 固定一页，否则按序循环。

| 阶段 | 覆盖 |
|---|---|
| `legacy-charsets` | C0、DECALN、G0 图形、VT52、保存光标、DEC 行属性 |
| `cursor-and-editing` | CUP/HVP、相对移动、ICH/DCH/ECH、EL、REP、IL/DL |
| `margins-and-scrolling` | DECSTBM、IND、RI、SU、SD |
| `legacy-sgr` | 基础属性、下划线变体、边框、上/下标与重置 |
| `extended-color` | ANSI 16、256 色、RGB 分号/冒号、下划线颜色 |
| `unicode-width` | CJK、组合字符、双宽、歧义宽度、emoji/ZWJ、边缘换行 |
| `cell-geometry` | 框线、方块、象限、盲文、Nerd/Powerline |
| `modern-osc` | 标题、OSC 8 链接 |
| `modern-modes` | 光标、应用/原点/自动换行、鼠标、焦点、粘贴、Kitty、modifyOtherKeys |
| `inline-graphics` | Kitty RGBA、iTerm2 PNG、Sixel（后两者应被安全忽略，见 [不支持的功能](../rendering-scrollback.md#7-不支持的功能)） |

每页首帧与尺寸变化时整页重绘，先复位链接、图形、边距、鼠标、焦点、粘贴与 Kitty 状态。`--sync` 时每帧包在 2026 中。输入编码不在本工具范围（见 [输入协议](../input-protocols.md)）。

## 4. 构建与运行

```cmd
:: 构建、运行单元测试并部署 Qt；产物在构建树 terminal-stress-tui
powershell -NoProfile -ExecutionPolicy Bypass -File tools\terminal_stress_tui\build.ps1

:: 在新 SpanTui 测试窗口中运行（参数透传）
tui_test.bat --scenario omp-transcript --seed 7 --jank 60

:: 在已打开的终端中运行
terminal-stress-tui.exe --scenario fullscreen-churn --fps 120 --stall-max-ms 800
terminal-stress-tui.exe --list
```

对照：在 Windows Terminal 中以相同 `--seed` 运行。两边都撕裂，是 conhost 按绘制节奏切开了帧；只有 SpanTui 撕裂，查 Core 发布切点（[渲染 §1](../rendering-scrollback.md#1-场景图渲染与快照交换)）。以 `--trace-viewport` 启动时，`viewport.jsonl` 的 `forcedCut` 标出被强制切开的发布。

代码：`stress_tui.cpp`（场景与写入计划）、`protocol_stages.cpp`、`main.cpp`；测试 `tools/terminal_stress_tui/tests/stress_tui_test.cpp`，由 `build.ps1` 运行。
