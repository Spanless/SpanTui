# 进程包含

`src/platform/windows/process_containment.*`：

| 函数 | 作用 |
|---|---|
| `installApplicationContainment()` | `main` 启动即创建带 `JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE` 的应用 Job 并加入当前进程；之后的子进程（含 Git）自动继承 |
| `startOwnedProcess(program, args, cwd)` | 启动须随主程序退出的辅助进程 |
| `startUnaffiliatedProcess(program, args, cwd)` | `CREATE_BREAKAWAY_FROM_JOB`，只用于须在主程序退出后继续运行的程序：更新安装程序，以及项目“打开方式”启动的外部编辑器 |

- 主进程以任何方式结束（含崩溃、被强杀），内核终止 Job 内全部子进程。
- 受管子进程不得用 `QProcess::startDetached`，否则会逃出 Job。
- 每个终端会话另有自己的 Job，关闭即终止该会话的整个进程树，不影响其他会话（见 [ConPTY 生命周期](../../terminal/conpty-lifecycle.md#3-退出与销毁)）。

测试：`process_containment_test`（参数引号往返：空格、引号、结尾反斜杠、空参数、非 ASCII；无法启动时返回系统错误文本）。
