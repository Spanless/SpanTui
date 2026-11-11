# 文档标签与外部变更

每个查看器瓦片一个 `DocumentManager`（`src/documents/document_manager.*`，`QAbstractListModel`）。查看器只读，不写回。

## 1. 持久化与恢复

- 标签与光标存 `document_tabs`（见 [数据库](database-schema.md)），以 (`owner_project`, `owner_tile`, `path`) 隔离；不存内容，内容总从磁盘读。
- 重启按 `sort_order` 恢复。文件不存在时标签保留、内容为空并标为缺失（`missing` 只在内存，每次恢复重新判定）。
- ≥ 1 MiB 的文件在恢复与打开时都走 `readDocumentAsync`（`QtConcurrent::run`）：标签先以空内容标为加载中并报告进度，读完在 GUI 线程填入；期间标签被关闭或外部变更代次已前进则丢弃结果。小文件同步读取。外部变更刷新用同一阈值（`kAsyncRefreshBytes`）。
- 读取时 `CRLF`（或单独 `CR`）归一为 `LF`；不写回，不记录原始行尾。

## 2. 外部变更（`document_monitoring.cpp`）

- `QFileSystemWatcher` 监视全部打开文件，每路径独立代次（`m_externalChangeGenerations`），批量改动（如切分支）不丢通知。
- 内容变化则重载；删除则置 `missing`，本次会话保留上次文本。
- Qt 的 Windows 后端只在修改时间、权限或属主变化时报告，不比较大小。NTFS 写入时间约 15.6 ms 前进一次（FAT32 为 2 s），同一刻度内的改写不产生通知，直到下一次时间不同的写入。

代码：`document_manager.cpp`、`document_persistence.cpp`、`document_monitoring.cpp`；测试：`document_manager_test`。
