# libghostty-vt 来源与集成规范

本文档记录 SpanTui 终端核心使用的第三方终端模拟库 `libghostty-vt` 的上游来源、构建方式与集成边界。

---

## 1. 上游来源与版本信息

- **官方上游仓库**：[ghostty-org/ghostty](https://github.com/ghostty-org/ghostty)（`include/ghostty/vt.h` 的 C API）
- **固定 Commit SHA**：`ae0dc1b7136c2baddc3e067663aa041cd555cfda`
- **构建工具**：Zig 0.17.0，`-Doptimize=ReleaseFast`，目标 `x86_64-windows-gnu`
- **静态库 SHA-256**：`CCF2649B4F17B9ADAC04B943EBB5F6A10A71B94FD9C864A3BA3FA32A0A10A139`（`lib/libghostty-vt.a`）
- **开源许可证**：MIT License（正文位于 `src/terminal/vendor/libghostty-vt/LICENSE`）

---

## 2. 预编译库与构建边界

- **提交预编译库**：`src/terminal/vendor/libghostty-vt/` 只含 `LICENSE`、`include/ghostty/` 头文件与 `lib/libghostty-vt.a`；日常构建与 CI 不需要 Zig。
- **CMake 目标**：`src/terminal/CMakeLists.txt` 将其声明为导入静态库 `SpanGhosttyVt`（`GHOSTTY_STATIC`，额外只依赖 `ntdll`），私有链接进 `SpanTerminal`；直接包含 `ghostty_engine.h` 的测试显式链接它。
- **封装边界**：只有 `src/terminal/core/ghostty_engine.*` 包含 `ghostty/vt.h`；其余代码只看到 `Frame`、`Line`、`Cell` 等本项目类型。
- **升级**：执行 `tools/update-libghostty.ps1 -Commit <sha> -Zig <zig.exe>`。脚本在 `.dev/tmp/` 下克隆并编译，用 `llvm-strip` 去调试信息、用 `llvm-objcopy` 去掉 GNU ld 不识别的 `.drectve` 段，再用 `llvm-ar` 按原顺序以纯文件名重打包（Zig 以构建缓存里的绝对路径给归档成员命名，不重打包会把本机路径带进提交），替换 `include/` 与 `lib/`，并打印新的 SHA-256；升级后同步更新本节的 commit、Zig 版本与哈希。`tools/check-docs.ps1` 的 `vendor` 规则核对本节与 [ConPTY](conpty.md) 记录的哈希等于仓库内二进制。

---

## 3. 源码补丁

无。引擎行为以上游为准，缺陷提交上游；宿主特有行为（win32-input-mode、PNG 解码、配色报告、ConPTY resize 选项）在 `GhosttyEngine` 与 CoreWorker 里经公开 API 接线，见 [终端总览](../services/terminal/overview.md)。

---

## 4. 相关代码与测试索引

- **封装引擎**：`src/terminal/core/ghostty_engine.cpp`
- **Vendor 目录**：`src/terminal/vendor/libghostty-vt/`
- **重新编译脚本**：`tools/update-libghostty.ps1`
- **自动化测试**：`tests/terminal/ghostty_engine_test.cpp`
