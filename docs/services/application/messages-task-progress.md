# 消息与任务进度

## 1. 消息中心（`MessageCenter`）

来源：领域错误、Qt 日志（warning/critical）、未捕获异常。呈现：Toast 堆栈（`MessageToastStack.qml`）与消息中心弹层（`MessageCenterPopup.qml`）。

### 告警完整性

- 数据根确定后立即安装启动日志与消息汇聚。
- 非空 `QtWarningMsg`/`QtCriticalMsg` 全部进入消息中心：不按类别、来源、正文过滤，warning/error 不去重；不用 `QLoggingCategory` 关闭 warning 类别。噪声在产生源修复。
- 相同 Info/Success 在 2 秒内去重。

### 排队投递

日志处理器转入的消息一律经事件循环排队，不在产生日志的调用栈内改模型；`postOnGui` 执行中再到达的消息同样排队。否则模型消费者（如 `rowsInserted` 触发的委托绑定）产生的告警会嵌套插入，而告警不可过滤，无法止住。排队只改时机，不丢弃。

- 日志处理器可在任意线程运行：取当前实例与排队在同一把可重入锁内；析构时在该锁下清空实例。
- 测试中断言"消息已出现"前须先让事件循环运行。

### 来源接线

- **领域错误**：SettingsStore、TerminalSettings、Translator、TerminalManager、ProjectWorkspaceRegistry、RemoteHostRegistry、WorkspaceController 的 `errorOccurred` 各自经运行时的 `reportErrors` 接到一次 `qCritical`，由日志路径投递——不再显式 `post`，避免重复。
- **领域模型**：文件树、Git、文档、远程文件模型经 `workspace_messages.cpp` 的 `connect*Messages` 直接投递：非空错误一条 Error，完成的文件操作一条 Success。连接以项目/主机上下文为生命周期。
- **终端通知**：OSC 9 / OSC 777 经 `TerminalManager::notificationRequested` 由 `connectTerminalHostEvents` 投递为 Info；无标题时用会话标题。
- **异常**：`GuiApplication::notify()` 捕获未处理的 `std::exception`，转为错误消息。

### 持久化

- 完整工作区迁移完成后，`MessageCenter` 以独立连接接管 `messages` 表，最多 200 条；之前的消息在内存中按序补写。恢复的历史不弹 Toast。独立终端不写。
- 写入按序入队，250ms 单发定时器在一个事务内提交并裁剪；析构时同步冲刷。条目 id 只在进程内有效，写库时映射到自增行 id，多进程共用数据库不冲突。任一写失败整批回滚并停用持久化。

### 呈现

四级：`Info`、`Success`、`Warning`、`Error`。新消息弹 Toast，驻留 5 秒后淡出；消息本体保留。条目正文默认折叠 3 行，可展开；复制按钮复制标题与正文。

## 2. 任务进度（`TaskProgressService`）

### 所有者（`TaskProgressBinding`）

| `ProgressOwner` | 绑定对象 | 场景 |
|---|---|---|
| `Git` / `GitHistory` / `GitDiff` | `GitRepositoryService` | 仓库发现与分支 / 历史 / 差异与提交详情 |
| `Update` | `UpdateService` | 检查；安装时下载、校验、启动按权重合成一条（见 [应用更新](../update/updater.md)） |
| `Files` | `FileTreeModel` | 跨卷递归复制与移动 |
| `Document` | `DocumentManager` | 1 MB 以上文件分块加载 |
| `Remote` | `RemoteTransferQueue` | sftp 上传下载批次 |

- 所有者统一发 `progressStarted(title)`/`progressChanged(fraction, detail)`/`progressFinished(outcome, detail)`，由 `bindProgress(service, owner, source)` 一次接好；binding 以所有者为父对象。组合根只在上下文创建信号（`contextCreated`、`editorDocumentsCreated`，均带具体类型指针）里调用它。
- 实例键为 `<source>#<owner>`，并发的多项目、多瓦片互不覆盖。binding 析构时把未完成的实例以 `Cancelled` 收敛。
- Git 按操作分源：`GitRepositoryService` 的 `taskStarted/taskFinished` 只带操作名（不依赖应用层），`bindGitProgress()` 用 `taskProgressSourceForGitOperation()` 映射语义源、`gitProgressTitle()` 生成标题（`task_progress_titles.h`，`tr()` 翻译）。开始与结束由同一操作名派生，不会错配；`commit-details`/`commit-files`/`commit-files-stats` 归 `GitDiff`。
- 终端进度（OSC 9;4）不经 binding：`connectTerminalHostEvents` 以 `terminal#<会话 id>` 直接驱动，不定进度为 -1；清除、退出或关闭时 `finish`，最后为出错则 `Failed`。独立终端不接入。

### 时序

- `kRevealDelayMs` = 160：更快完成的任务不显示。
- `kFinishedLingerMs` = 1600：已显示的任务结束后保留结果 1.6 秒。

## 3. 代码与测试

- `src/application/{message_center, task_progress, task_progress_binding, workspace_messages, terminal_host_events, gui_application}.cpp`
- QML：`qml/components/business/messages/`
- 测试：`message_center_test`（排队、重入、落盘）、`message_center_wiring_test`（真实接线）、`task_progress_test`
