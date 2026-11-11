# 视口事务

窗口拉伸、最大化、Snap、最小化还原、DPI 切换与接缝拖动产生连续几何变化。`WindowTransitionCoordinator`（`src/application/window_transition_coordinator.*`）把它们收敛为带代次（epoch）的事务，避免各组件各自 resize 造成闪烁、纹理重建与终端重排撕裂。

## 1. 状态机

```text
Stable → InteractiveResize / StateChange（epoch+1，冻结高开销效果，previewViewport）
       → Settling（120ms 无变化）→ commitViewport(epoch) → Stable
```

| Phase | 含义 |
|---|---|
| `Stable` | 无未提交变化 |
| `InteractiveResize` | `WM_ENTERSIZEMOVE`…`WM_EXITSIZEMOVE`，或拖动 QML 接缝 |
| `StateChange` | 最大化、还原、Snap、程序化 resize |
| `Minimized` | 停止渲染，保留最后画面 |
| `Settling` | 120ms 防抖（`kSettleMilliseconds`） |
| `Recovering` | 还原后等待首个稳定帧 |

停滞看门狗（`m_stallWatchdog`，4 秒）在交互事件长时间静默时强制提交，防止丢失 `WM_EXITSIZEMOVE` 永久冻结；原生拖拽仍持有鼠标 capture 时视为暂停而非丢失，不接管。

异步操作、终端 resize 与 GPU 提交都携带 epoch；旧 epoch 的响应只能清理自身资源，不得改动当前尺寸。

## 2. 两阶段布局

事务代次只由协调器持有；`WorkspaceLayout` 只记"事务是否打开"，由协调器按序同步驱动（`epochChanged` → `beginViewportTransaction`，`viewportPreviewed` → `previewViewport`，`viewportCommitted` → `commitViewport`），不另存 epoch。

- 预览：`WorkspaceLayout::previewViewport(width, height)` 只重算瞬态坐标并发 `geometryChanged`，不写盘。
- 提交：`commitViewport()` 关闭事务，有待提交的变化时更新比例并发 `layoutCommitted`；持久化只响应 `layoutCommitted`。

## 3. 终端 resize

`TerminalSession` 记下最新视口并按序投给 Core；Core 先换网格，再通知进程（流程见 [ConPTY 生命周期](../../services/terminal/conpty-lifecycle.md#2-视口调整resize)）。

- 只有正在呈现的视图（持有渲染消费者、可见、窗口未隐藏或最小化）提交网格；隐藏视图的提交保留到它成为活动消费者时。
- 进程端只保留最新尺寸，一次只提交一个 `ResizePseudoConsole`；失败经 `MessageCenter` 告警，不回滚网格。
- 转换期间 View 保持最后帧；View 的快照接受只采用匹配当前视口 epoch 的快照（滚动是 View 本地状态，不参与比对）。

## 4. 内容冻结

- `ContentContainer` 按拖动、分屏、缩放把终端标记为 moving/closing/hidden（`contentLifecycle`），`TerminalSurface` 暂停快照接收与重排，稳定后恢复。
- `PreviewPane` 在事务期间冻结 Markdown/代码布局（`resizeDeferred`），稳定后一次提交。
- 拖动期间暂停光标呼吸与窗格过渡。

`Minimized` 时卡死看门狗放慢心跳，见 [UI 卡死看门狗](../../services/platform/windows/diagnostics-crashes.md#3-ui-卡死看门狗uihealthmonitor)。
