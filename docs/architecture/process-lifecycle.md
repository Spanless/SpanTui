# 进程生命周期

```text
全局应用 Job（KILL_ON_JOB_CLOSE）
 ├─ SpanTui.exe
 │   └─ git.exe 等 QProcess 子进程（继承应用 Job）
 ├─ 会话 Job A ── conhost.exe ── 用户 Shell 进程树
 └─ 会话 Job B ── ...
更新安装程序 ── CREATE_BREAKAWAY_FROM_JOB，脱离应用 Job
```

1. **应用 Job**：`src/main.cpp` 调用 `span::platform::process::installApplicationContainment()`，主进程以任何方式退出时内核回收全部子进程。
2. **会话 Job**：`WinConPtyProcess`（`src/terminal/platform/windows/win_conpty_process.cpp`）为每个会话建独立 Job，关闭会话即终止该会话启动的全部进程，不影响其他会话。
3. **Git**：`GitLaneScheduler`（`src/git/git_lane_scheduler.cpp`）用 `QProcess` 启动 `git.exe`，继承应用 Job；全部在后台车道异步执行，GUI 线程不调用 `waitForFinished()`。
4. **更新安装程序**：应用 Job 内启动的进程会在主程序退出时被杀死，因此 `startUnaffiliatedProcess()`（`src/platform/windows/process_containment.cpp`）以 `CreateProcessW` + `CREATE_BREAKAWAY_FROM_JOB` 启动，脱离失败即报错、不降级。项目“打开方式”启动的外部编辑器同样经它脱离 Job，关闭 SpanTui 不结束编辑器。安装程序以 `/WAITPID=<pid>` 等主程序释放文件，替换后以 `/RELAUNCH=1` 启动新版（见 [应用更新](../services/update/updater.md)）。
