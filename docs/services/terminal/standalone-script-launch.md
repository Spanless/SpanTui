# 独立终端与脚本运行

## 1. 独立终端

```cmd
SpanTui.exe --terminal --cwd "C:\projects\app" [--command "npm start"]
```

由 `src/application/runtime/terminal_application.cpp` 组装，加载 `qml/TerminalPanel.qml`。不走单实例路由，每次一个新进程；不读写工作区数据库、不持久化会话；关闭窗口即销毁会话 Job 并退出。

## 2. 文件树运行脚本

文件树右键脚本 →「运行脚本」（双击仍是打开查看）。哪些文件可运行、怎样运行由平台决定（`platform/script_launch.h` 的 `scriptCommandLine`）；路径含双引号或换行时不可运行：

| 平台 | 脚本 | 命令 |
|---|---|---|
| Windows | `.bat`/`.cmd`/`.ps1` | `powershell.exe -NoExit -ExecutionPolicy Bypass -Command "& '<脚本>'"` |
| macOS | `.sh`/`.command`（`/bin/sh`）、`.bash`、`.zsh` | `<登录 shell> -l -c "<解释器> '<脚本>'; exec <登录 shell> -l"`，登录 shell 取 `$SHELL`，否则 `/bin/zsh` |

脚本结束后都保留交互式 shell。

1. `FileTreeModel::canRunInProjectTerminal(row)`：项目内的文件且平台给出命令。
2. `FileTreeModel::projectTerminalCommand(row)` 返回该命令。
3. 按 `workspace.scriptRunMode`：`projectSession`（默认）经 `SessionModel::addCommandSession` 在工作区新建终端瓦片；`independentWindow` 启动 §1 的独立终端。

命令只作为本次运行参数，不保存为会话的启动命令。

测试：`tests/application/launch_options_test.cpp`、`tests/workspace/file_tree_model_test.cpp`（Windows 脚本）、`tests/platform/script_launch_test.cpp`（macOS 脚本）。
