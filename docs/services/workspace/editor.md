# 文件查看器

只读：复制、查找、跳转行、行号、Markdown 预览、Git 行状态色条。

```text
PreviewPane.qml
  ├ CodePreview.qml       普通文本
  │   ├ EditorController  查找、跳转行、与 HEAD 比对
  │   │   └ CodePreviewLineModel  行模型（text / gitStatus / deletionBefore）
  │   └ TextLinesView     mode: file
  └ MarkdownPreview.qml   见 [Markdown](markdown.md)
```

文本与标签由 `DocumentManager` 提供，磁盘变更自动重载，查看器不写回。

## 1. 行模型

`TextLinesView` 同时用于文件（`file`：行号与色条）与 Git diff（`unified`/`split`，行为 JS 数组）。文件模式的行来自 `CodePreviewLineModel`（`src/documents/code_preview_line_model.*`）：

- `setText` 按 `\n` 拆行并重置模型，相同文本不重置；同时算出 `maxColumns`（制表位 4）供横向宽度使用。无文档时 `clearDocument()` 为 0 行；空文件为 1 个空行。
- `setBaseContent` 后经 180ms 防抖与 HEAD 逐行比对，只对状态变化的连续行段发 `dataChanged`。`clearBaseContent()` 只清已标记行的状态。
- 没有基线（非 Git 项目或尚未返回）时不标记、不排比对；未跟踪文件要等基线返回 `existsInHead == false` 才整体标为新增。

行高 = 编辑器字号的 `FontMetrics.height` × `editorLineHeight`，不用固定像素，否则大字号或大行距会裁字。

## 2. 查找与快捷键

查找与跳转行由 `EditorController` 对当前文本计算。查找栏与终端共用同一组件，`EditorSearchBar` 另加跳转行输入框。快捷键经 `Shortcuts.matches()` 判定，共用组合时依次全部执行（查找最后，焦点落在查找栏），见 [快捷键](../application/settings-appearance.md#快捷键)。

测试：`editor_controller_test`。
