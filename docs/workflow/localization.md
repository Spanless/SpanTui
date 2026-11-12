# 国际化

## 1. 字符串

- 用户可见文本：C++ 用 `tr()`，QML 用 `qsTr()`。
- 不翻译：命令行参数、设置键、SQLite 列名、协议标识、枚举字符串、日志字段（按字面匹配）；语言选项名用各自语言书写（English、简体中文）。
- 译文保留 `%1` 等占位符；不用 `+` 拼接句子片段（语序因语言而异）。

## 2. 运行时切换

`Translator`（`src/application/translator.h/cpp`）监听 `AppearanceSettings::language`（`zh_CN`、`en`），加载 `:/i18n/SpanTui_zh_CN.qm`，再调用 `QQmlEngine::retranslate()` 重算全部 `qsTr()` 绑定，无需重启。

## 3. 维护 `translations/SpanTui_zh_CN.ts`

1. 手工在对应 `<context>` 下加 `<message>`：
   - context 为 QML 文件名，或 C++ 类的 moc 全名（含命名空间，如 `span::application::WorkspaceController`；裸类名不生效）；`QCoreApplication::translate("<名称>", …)` 以第一个参数为准；QML 首行 `pragma Translator: "<名称>"` 时以该名称为准。
   - `<source>` 与源码逐字一致；`<translation>` 填中文，不带 `type="unfinished"`。文件不含 `<location>`，手工添加不会与生成结果冲突。
2. `spantui_update_translations`（lupdate）可批量重建，日常不需要。
3. 静态检查 `tools/check-translations.ps1` 校验每个源串都有完成的译文且占位符一致、目录里没有不再使用的条目；C++ `tr()` 按所在类推出 context（含 `Q_DECLARE_TR_FUNCTIONS`、`X::tr()`），推不出时报错。它不用 lupdate 作参照：lupdate 对 `\t`、`\r` 等转义的解码与运行时不同，也认不出 `QCoreApplication::translate()` 的 context。
