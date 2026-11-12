# 终端子系统

基于 ConPTY 与 [libghostty-vt](../../legal/libghostty-vt.md)，全部在主进程内运行。

## 1. 拓扑与线程

```text
TerminalSurface.qml          右键菜单、滚动条、聚焦
  └ TerminalView             GUI 线程：输入、IME、选区请求、场景图
      └ TerminalFramePreparer   全部 View 共用一个线程（各持图集）：排版、图集、整行顶点
  └ TerminalSession          GUI 线程：会话状态机，持最新视口，委托 Input
      ├ TerminalFrameStore   不可变帧交换
      └ TerminalOutputQueue  1 MiB 有界字节队列
TerminalCoreWorker           每会话一个 QThread：独占 GhosttyEngine，解析与发布；网格变化后通知进程 resize
TerminalProcess              无专属线程：重叠读写、退出等待与 resize 都在系统线程池回调中执行
```

- 进程的 `start()`/`stop()` 会阻塞在操作系统上，Session 放到分离线程执行，GUI 线程从不等待。启动结果带 generation 排队回 GUI，过期结果只释放进程。

- Core 把视口及上下 overscan 行（平时各一屏，拖动滚动条时各一行）打包为 `TerminalFrameSnapshot`，行带 libghostty 行 id 与修订号供准备线程复用。
- `TerminalView::updatePolish` 在 GUI 线程接受匹配视口的已准备帧（只换指针）；`updatePaintNode` 在同步阶段调 `TerminalSceneGraph::sync`，只复制变化行的顶点。
- `TerminalOutputQueue`（`src/terminal/shared/`）：唤醒回调的撤销与执行在同一锁下串行；`append(bytes, endsWrite)` 保存写入边界，`take()` 不跨越边界并报告是否恰好结束一次写入。
- `TerminalFrameStore`：锁内交换 `shared_ptr<const TerminalFrameSnapshot>`，返回 `Rejected`/`Released`/`NotifyAgain` 合并中间帧；被跳过快照的 `fullRefresh` 要求带到下一份。

## 2. 局部所有者

- `TerminalSessionInput`：粘贴与输入积压、分片定时器、括号粘贴；经回调查询当前后端，不缓存指针。
- `TerminalPresentationScheduler`：Core 线程上发布时机的唯一所有者（见 [渲染](rendering-scrollback.md#1-场景图渲染与快照交换)）。
- `GhosttyEngine`：只在 Core 线程访问；新 transport 新建实例，不继承上一个子进程的状态。新建失败（`isReady()` 为 false）时保留旧引擎并发 `transportEpochFailed`，会话进入错误状态，不发布空帧。
- View 只持滚动动画位置与已准备行；选区、查找、提示符跳转都是发给 Core 的请求（见 [滚动历史](rendering-scrollback.md#4-滚动历史选区与查找)）。`TerminalInputRouting`、`TerminalSceneGraph` 分担输入路由与 QSG 同步；View 的输入处理（按键、鼠标、滚轮、IME、链接）在 `terminal_view_input.cpp`，快照接受在 `terminal_view.cpp`。

## 3. 身份与失效边界

进程回调 generation、视口 epoch、Core transport epoch、View binding generation、QSG scene generation 各属不同生命周期，互不替代。进程的 `cancelPendingWritesAndInterrupt` 只撤销未交付字节，不进帧。行 id/修订号（Engine）、整帧刷新修订（Worker）与 View 布局修订分层更新；准备线程只在 transport epoch 相同时按行 id 复用行。

停止、退出、启动失败与析构共用撤销排队工作的入口，但各自信号与代次语义不合并。

相关测试：`tests/terminal/` 下 `ghostty_engine_test`、`terminal_session_test`、`terminal_core_worker_test`、`terminal_manager_test`、`terminal_view_binding_test`、`terminal_input_routing_test`、`win_conpty_process_test`。手工压测见 [压测 TUI](lab/stress-tui.md)。

## 4. 本域改动完成标准

### 线程边界或同步原语
- [ ] 跨线程数据只经 `TerminalOutputQueue`、`TerminalFrameStore` 或 queued 信号；GUI 线程不读 Core 可变状态。
- [ ] 跨线程 `QFont` 经 `TerminalFontSet::isolatedCopy`/`isolate()`，不共享隐式数据。
- [ ] 新代次只由自身生命周期推进，不复用 §3 中其他层的计数。
- [ ] 停止、退出与析构路径都撤销新增的排队工作，并有 `terminal_session_test`/`terminal_core_worker_test` 覆盖。

### 引擎适配或升级 libghostty-vt
- [ ] 行为与 [渲染](rendering-scrollback.md) 一致；改单元格转换时同步其 §5。
- [ ] 不改 libghostty 源码；升级经 `tools/update-libghostty.ps1`，并更新 [libghostty-vt](../../legal/libghostty-vt.md) 的 commit、Zig 版本与哈希。
- [ ] 上游不支持的功能优先舍弃，记入 [不支持的功能](rendering-scrollback.md#7-不支持的功能)。
- [ ] 用 [压测 TUI](lab/stress-tui.md#3-协议覆盖protocol-场景) 的 `protocol` 场景复核。

### 发布时机
- [ ] 新规则写进 `TerminalPresentationScheduler` 并有虚拟时钟用例，Core 中不另加定时器。
- [ ] 输出触发的发布只在切点进行；强制切开有上限并计入 `forcedCut`。
- [ ] 用 `omp-transcript` 与 `fullscreen-churn` 场景复核无撕裂。

### 渲染与视图
- [ ] 只接受匹配当前视口的快照；resize 过程中不发布空帧。
- [ ] QSG 节点只在 `updatePaintNode` 中创建或修改。
