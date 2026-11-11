# 文件树

`FileTreeModel`（`src/workspace/file_tree_model.*`）：扁平节点数组（`m_nodes`）上的 `QAbstractListModel`。文件操作在 `file_tree_storage.cpp`。

## 1. 加载与刷新

- 启动只读根目录直接子项，展开时才读子目录；一个目录的子项一次插入。刷新时后台按先序构建整棵已挂载树。
- **增量刷新**：结构不变时只更新字段、不发信号；结构不同时按路径对齐两份先序列表，保留共有行（字段变化才 `dataChanged`），整段删除旧独有行、整段插入新独有行；路径换位置时先删后插。不 `beginResetModel`，视图保留委托与滚动位置；只有切换/清空根才重置。
- **图标**（`FileIconTheme`，`:/icons/file-icons.json`）：先匹配完整文件名（不区分大小写），再由长到短匹配复合扩展名，最后回落通用图标。
- **类别染色**（`CategoryRole`：build/test/dependency/docs/assets/config）：只看相对项目根的路径段，根外的目录名不影响。

## 2. 文件操作

- **按路径寻址**：`createFile`/`createFolder(parentPath, name)`、`renamePath`、`deletePath` 只接受路径，不接受行号（异步刷新后行号会指向别的节点）。父路径为空表示根；是文件则在其旁新建。
- **根内校验**：源与目标必须在 `projectRoot` 内；越出根的链接视为外部，读目录时跳过。根保存为规范路径。解析规范路径每次要开句柄，因此只对链接解析：读目录时只解析本身是链接的条目；校验路径时字面在根内且路径上无链接直接通过，否则解析整条路径再比较。
- 拒绝 Windows 保留设备名（`CON`、`PRN`、`AUX`、`NUL`、`COM1..9`、`LPT1..9`）；目标已存在则拒绝。
- **跨卷移动**：仅源与目标父目录不在同一卷时降级为复制删除——同卷 `rename` 失败说明有占用，直接报错。`movePathAsync` 在 `QtConcurrent::run` 中递归复制再删源，进度归 `Files`。删源失败时（`removeRecursively` 会跳过占用项继续删）先用副本补回源中缺失的条目再丢副本；补不齐则保留副本并在错误中给出路径，源与副本不会同时丢失。测试用 `setCopyMovesForTesting(true)` 在单卷上走这条路径。
- **删除与撤销**：移入 `<root>/trash/<timestamp>_<文件名>`，`undoLastDelete` 撤销一步。撤销栈只在内存，启动时 `purgeTrash` 清理上次遗留。

## 3. 监视与 Git 状态

- 只监视根与已展开目录。事件先过滤（`.git/` 内与根外直接丢弃），其余经 250ms（`m_watchCoalesce`）合并，到期时同步监视集合（集合去重）再刷新。外部递归删除已展开目录时由存活的父目录驱动刷新，失效 watch 在同步时移除；期间 Qt 打印的 `FindNextChangeNotification failed` 照常进入消息中心。
- **状态冒泡**（`gitStatusPriority`）：`conflict` 6 > `deleted` 5 > `modified`/`type-changed` 4 > `renamed`/`copied` 3 > `added` 2；目录显示子项最高级。

## 4. 脚本

`.bat`/`.cmd`/`.ps1`：`canRunInProjectTerminal`、`projectTerminalCommand`、`runInIndependentTerminal`（见 [脚本运行](../terminal/standalone-script-launch.md)）。

测试：`file_tree_model_test`。
