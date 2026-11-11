# 启动参数

## 1. 公开参数

`src/application/launch_options.cpp` 解析：

```text
SpanTui.exe [--project <dir>] [--safe-graphics] [--data-dir <dir>]
SpanTui.exe --terminal --cwd <dir> [--command <cmd>] [--safe-graphics] [--data-dir <dir>]
SpanTui.exe --help
```

| 参数 | 说明 |
|---|---|
| `--project <dir>` | 工作区模式打开该项目；不能与 `--terminal` 同用 |
| `--terminal` | 独立终端，不走单实例 |
| `--cwd <dir>` | 独立终端的初始目录，必须存在 |
| `--command <cmd>` | 独立终端启动后执行；该进程退出即结束会话 |
| `--safe-graphics` | 本次使用软件渲染（WARP），不写设置；已有实例运行时不生效（见 [图形启动](../services/application/startup-single-instance.md#图形启动与软件渲染)） |
| `--data-dir <dir>` | 本次数据根，不存在则创建，是文件则报错；优先于便携标记（见 [路径配置](paths-configuration.md#1-数据根productpaths)）。不同数据根的实例互不转发 |
| `--help` | 打印用法，退出码 2 |

路径参数均为绝对路径。

## 2. 内部参数

`src/application/runtime/internal_launch_options.cpp` 解析。`--isolated-workspace`、`--headless`/`--no-window`、`--resource-report` 与诊断开关在所有构建中生效；其余只在 Debug（未定义 `NDEBUG`）生效。

| 参数 | 说明 |
|---|---|
| `--isolated-workspace` | 数据库放在 `<数据根>\isolated\run-<uuid>`，退出删除；数据根本身不变，故开发工具总是同时传 `--data-dir` |
| `--headless` / `--no-window` | 无窗口运行 |
| `--resource-report=<path>` | CPU、内存、GPU 指标以 JSONL 追加写入 |
| `--conpty=system` | 改用系统 ConPTY，对照捆绑版本（见 [ConPTY](../services/terminal/conpty-lifecycle.md)） |
| `--trace-viewport[=1\|0]` | 覆盖视口追踪默认值（Debug 开、Release 关） |
| `--boundary-gallery-shot=<path>` | 加载 `BoundaryGallery.qml`，渲染后截图 |
| `--boundary-gallery-theme=<light\|dark>` | 边界画廊主题 |

工作区截图（任一参数即进入截图模式：隔离数据、固定视口、延时截图后退出）：

| 参数 | 说明 |
|---|---|
| `--workspace-shot=<path>` | 输出路径 |
| `--workspace-shot-theme=<light\|dark>` | 主题 |
| `--workspace-shot-project=<path>` / `--workspace-shot-file=<path>` | 预先打开的项目与文件 |
| `--workspace-shot-viewport=<宽x高>` | 默认 `1600x900` |
| `--workspace-shot-page=<workspace\|settings>` | 页面 |
| `--workspace-shot-right-tool=<files\|diff\|history>` | 右栏页 |
| `--workspace-shot-terminal` | 创建一个 `cmd.exe /Q /K` 终端 |
| `--workspace-shot-delay-ms=<ms>` | [500, 30000]，默认 1600 |

`dev.bat run --smoke` 附加 `--data-dir <仓库>\.dev\data --isolated-workspace`，进程在限定时间内存活即输出 `SMOKE_OK`。测试：`tests/application/launch_options_test.cpp`。
