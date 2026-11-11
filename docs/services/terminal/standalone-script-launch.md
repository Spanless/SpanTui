# 独立终端与脚本运行

## 1. 独立终端

```cmd
SpanTui.exe --terminal --cwd "C:\projects\app" [--command "npm start"]
```

由 `src/application/runtime/terminal_application.cpp` 组装，加载 `qml/TerminalPanel.qml`。不走单实例路由，每次一个新进程；不读写工作区数据库、不持久化会话；关闭窗口即销毁会话 Job 并退出。

## 2. 文件树运行脚本

文件树右键 `.bat`/`.cmd`/`.ps1` →「运行脚本」（双击仍是打开查看）：

1. `FileTreeModel::canRunInProjectTerminal(row)` 判定可运行。
2. `FileTreeModel::projectTerminalCommand(row)` 生成 `powershell.exe -NoExit -ExecutionPolicy Bypass -Command "& '<脚本>'"`，脚本结束后保留提示符。
3. 按 `workspace.scriptRunMode`：`projectSession`（默认）经 `SessionModel::addCommandSession` 在工作区新建终端瓦片；`independentWindow` 启动 §1 的独立终端。

命令只作为本次运行参数，不保存为会话的启动命令。

测试：`tests/application/launch_options_test.cpp`。
