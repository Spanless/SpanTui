# SpanTui 开发入口

本文件只写开发流程与本项目特有的约定；领域细节在 `docs/`，按需读取。文档与实现冲突、或两套实现互相矛盾时，不自行补全结论，在交付说明中列出冲突双方与证据，交维护者裁决。

---

## 一、安全底线

1. **保护工作树**：开始前执行 `git status --short`；既有未提交内容属于维护者，不用 `git reset --hard`、`git clean -fd`、`git checkout --` 清理。
2. **修改要有依据**：缺陷修复需要可复现问题、失败日志、失败测试或明确的源码不变量；审计类任务保持只读。
3. **最小完整改动**：只改任务需要的文件，但迁移全部调用方并删除被替代路径，不留新旧并行实现或兼容空壳。
4. **告警不隐藏**：非空 `QtWarningMsg`/`QtCriticalMsg` 全部进入 `MessageCenter`，不按类别、来源或正文过滤，不去重；噪声在产生源修复。日志进入 UI 一律排队投递——同步回灌会让模型消费者产生的告警嵌套插入，而告警不可过滤意味着没有保险丝（见 [消息与任务](docs/services/application/messages-task-progress.md)）。
5. **输出隔离**：排查输出写入已忽略的 `.dev/debug/`，测试日志与临时文件写入 `.dev/` 其余子目录（或构建树）；不写系统 TEMP、`%APPDATA%` 等仓库外位置。开发运行一律带 `--data-dir <仓库>\.dev\data`（`dev.bat`/`start.bat` 已自动附加）。根目录条目、产出位置与本机配置的规范见 [仓库结构](docs/workflow/overview.md#1-仓库结构)，`tools/check-layout.ps1` 检查。
6. **Git 写操作需明确指示**：暂存、提交、推送、建删分支、变基、打标签、发布；提交授权不含推送。`master`、`dev` 只经 Pull Request 变化，不直接推送（[分支模型](docs/workflow/release.md#1-分支模型)）。
7. **构建与测试只在收尾做一次**（细则见 [验证门禁](docs/workflow/verification.md)）——构建很慢，改一步测一步会让迭代慢到不可接受：
   - **静态检查**（随时，免授权）：`tools/agent-check.ps1`、`tools/check-*.ps1`、`git diff --check`；提交前钩子（`.githooks/pre-commit`）对暂存内容运行 `agent-check`；
   - **收尾验证**（任务完成时，免授权）：一次增量 `dev.bat build`、回归测试 `dev.bat test -R <受影响测试>`，`check-ownership` 列出时再加无窗口 `dev.bat run --smoke`；改动过程中不构建、不跑测试；不跑全量测试，除非维护者要求；
   - **排查失败测试**：只跑出错的单个测试函数 `dev.bat test -R <测试> --function <函数> [--repeat N]`，不反复跑整组；测试一律无头（offscreen）运行，不在维护者桌面弹窗口（见 [测试设计规范](docs/workflow/testing.md#4-测试环境隔离与安全约束)）；
   - **需授权操作**（逐条申请）：窗口验证（通常由维护者人工完成）、依赖安装、`dev.bat package`、提交代码。

   构建与测试只经 `dev.bat`，不直接调用 `cmake --build`/`ctest`（原因见 [构建规范](docs/workflow/build.md)）。
8. **文档同轮同步**：行为、接口、配置、持久化格式变化时同轮更新主责文档；技术文档只放在 `docs/`，不建"新版""草稿"等并行副本。

---

## 二、工作流程

1. **动手前**：运行 `tools/check-ownership.ps1 -Files <计划改动的路径>`，阅读它列出的文档；它标为高影响的对象，改 API 或行为前先全库搜索调用方。需要整体结构时读 [架构总览](docs/architecture/overview.md)。工作树已有未提交内容时，先跑一次 `tools/agent-check.ps1` 作基线，之后只处理新增的 WARN。
2. **改动中**：只做静态检查。
3. **收尾**：`tools/agent-check.ps1` 输出 `AGENT_CHECK_OK`；它的 WARN 表示代码变了而主责文档没动——文档描述的行为变了就同步，没变就在交付说明里写一句理由；对照所读文档中的"本域改动完成标准"节（通常在领域入口文档末尾）；然后做收尾验证——按 `check-ownership.ps1 -Changed` 输出的"Final verification"逐条执行。失败时先修完全部问题再重跑，不逐个修逐个跑。
4. **窗口验证交给维护者**：除非维护者明确要求，不打开可见窗口或截图比对；交付时注明未做视觉验收，以及需要维护者查看的界面。

---

## 三、项目约定

- **分层**：六层单向依赖，第 1–4 层不依赖 `src/application/` 与 QML 类型（[架构总览](docs/architecture/overview.md)）。
- **状态**：每个状态只有一个所有者（[状态所有权](docs/architecture/state-ownership.md)）；QML 是纯展示层，不持业务状态、不直接操作文件/进程/数据库。
- **命令与异步**：跨模块命令经 `WorkspaceController` 路由；异步结果校验 generation/epoch，陈旧结果丢弃。
- **常量**：契约常量收敛到模块常量头 `<模块>_constants.h`，文件内调优值在文件顶部命名，不散落裸字面量（[常量](docs/architecture/overview.md#常量)）。
- **持久化**：数据库结构只经 `WorkspaceDatabaseMigration` 变更（[数据库模式](docs/services/storage/database-schema.md)）。
- **UI**：组件三层 `business → common → base`，business 不直接实例化 base；弹层走 `AppPopup` 体系，行走 `ListItem`/`NavRow`，文本走 `UiText`/`MonoText`/`TerminalText`；视觉值走 Theme token；用户文案用 `qsTr()`，同轮直接手工编辑 `.ts` 补中文译文（见 [国际化规范](docs/workflow/localization.md)；[UI 层总览](docs/ui/overview.md)）。
- **并行任务**：每项任务一个 git 工作树（`dev.bat worktree add <分支>`），不在同一检出里切分支或用 stash 搬运未提交改动（[并行开发](docs/workflow/overview.md#3-并行开发多工作树)）。
- **测试**：断言公开接口与真实副作用，不扫描源码文本、不以像素代测行为；数据目录、Git 远端、PTY 均用隔离夹具（[测试设计规范](docs/workflow/testing.md)）。

---

## 四、索引

全部技术文档见 [docs/index.md](docs/index.md)；路径与数据目录见 [paths-configuration](docs/reference/paths-configuration.md)，术语见 [glossary](docs/reference/glossary.md)。
