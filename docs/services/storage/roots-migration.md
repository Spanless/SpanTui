# 数据根迁移

数据根解析与目录清单见 [路径配置](../../reference/paths-configuration.md)（`ProductPaths`，`src/core/product_paths.*`）。

## 1. 旧默认数据根

旧版默认数据根为 `%APPDATA%\Span\SpanTui`。`main` 在设好 `--data-dir` 之后、崩溃处理与启动日志创建数据根之前调用 `ProductPaths::moveLegacyDefaultRoot()`：

- 仅当使用默认数据根（无 `--data-dir`、非便携）、`%LOCALAPPDATA%\SpanTui` 不存在而旧目录存在时，整体改名过去并删除变空的 `%APPDATA%\Span`。
- 新目录已存在时两边都不动。
- 改名失败（跨卷、占用）时保留旧目录，本次用空的新数据根；警告在 `MessageCenter` 接上日志后发出，写明两个目录。

## 2. 旧设置文件

旧版 `QSettings` 写在 `<root>\Span\SpanTui.ini`。`ProductPaths::installApplicationSettings()` 在日志就绪后、任何设置读取前：

- 旧文件存在且 `settings.ini` 不存在时改名过去，删除变空的 `Span\`；`settings.ini` 已存在时不再读旧文件。改名失败记 warning，本次按无设置运行。
- 把 Qt 默认 `QSettings` 绑到 `<root>\settings.ini`（INI 格式），遗漏的默认构造不会写注册表。

测试：`product_paths_test`。
