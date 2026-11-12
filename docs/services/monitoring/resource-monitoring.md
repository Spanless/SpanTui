# 资源监控

## 1. 采样（`ResourceMonitor`）

`src/monitoring/resource_monitor.*`：

- **目标来源**：每轮采样前向宿主要一次要测量的会话（`SessionTargetSource`：会话 id、根进程、作业进程、是否在运行）。监控库不依赖终端库；工作区组合根把 `TerminalManager` 经 `src/application/terminal_resource_targets.*` 转成目标。
- 定时器（`performanceMonitorIntervalSeconds`）把采样投递到后台单线程（`ResourceSampleProvider`），同时最多一个在途，上一次未完成则跳过本次；结果以不可变快照交给 GUI。
- **按需激活**：显示读数的界面以 `setConsumerVisible(consumer, visible)` 登记（按消费者记集合，不计数）。有可见消费者时 `setActive(true)`，立即采一次并启动定时器；最后一个隐藏或销毁时停表。未激活时读数为 0，不调用 `sessionMetrics`。
- **空闲退避**：`IdleGovernor`（窗口未激活或 30 秒无输入）使间隔 ×5；恢复活跃且处于激活状态时立即补采。
- **指标**：`GetProcessMemoryInfo`（私有字节、工作集）、`GetProcessTimes`（CPU，记录上次值，进程树成员变化不产生尖峰）、`GlobalMemoryStatusEx`（物理内存，作内存等级的分母）。GPU 经 PDH 英文计数器 `\GPU Engine(*)\Utilization Percentage`，`gpu_engine_metrics.cpp` 解析实例名中的 `pid_<n>` 汇总会话进程树；不支持或无权限时只缺 GPU 项。

远程终端改用该主机的 `RemoteSystemMonitor`（见 [SSH 远程 §6](../remote/ssh-remote.md#6-服务器资源读数)）。

## 2. 标题栏读数（`ResourceStrip.qml`）

本地与远程共用：

- **摘要**：一个 `ChromeButton`（`ghost`，详情打开时 `selected`）：负载圆点、「CPU x%」「内存 y」（本地为会话进程树工作集，远程为服务器内存占比）与下拉箭头（详情打开时换成向上的 `chevron-up`，不用旋转——旋转后的图标不做像素对齐），悬停与按下底色覆盖整体，点击开关详情。标签与数字同字号，数字紧跟标签（间距 `spaceSm`）、左对齐。数字槽宽 = max(当前读数, 本次显示以来的最大读数宽)：只增不减，所以少一位数时右侧留空而不是把后一项拉回来；读数隐藏后棘轮归零。不按典型读数预留宽度——`0%`、`19 MB` 这类常见读数会因此在数字后空出一段，头部本来就紧。旧实现按最宽读数（`100%`、`88.88 GB`）占位并右对齐，常见读数下标签与数字之间空出一大段。
- **首次读数**：本地摘要在采样轮次覆盖到该会话之前不显示——此前 `sessionMetrics()` 为空，会先闪出「CPU 0% 内存 0 B」。`sampled` 直接看会话样本是否存在（不想要读数时样本视为空），不看读数的变化：读数控件可能在采样开始后才创建，空闲 shell 的工作集也可能几分钟不变，等"变化"就会一直不显示。会话样本 `perf` 不是绑定，而是在每轮 `metricsChanged`、会话切换与登记变化时调用 `refreshPerf()` 重读：`sessionMetrics()` 是普通调用，绑定只能靠把监视器的属性读进一个不用的局部变量来订阅，而 QML 编译器（qmlcachegen）会删掉这种读取，读数就永远收不到样本。远程摘要在服务器可用前不显示。
- **登记**：终端运行且瓦片聚焦或已固定（`active`；点头部的分支按钮也会聚焦该瓦片）、且头部有空间（宿主未置 `collapsed`）时登记为消费者；收起时不显示也不采样，并关闭详情、取消固定。头部收缩顺序见 [工作区组合 · 终端头部](../../ui/layout/workspace-composition.md#终端头部)。
- **等级**：正常 < 60%、偏高 ≥ 60%、过高 ≥ 85%；圆点取最差一级，两个数字各按自身等级着色（正常为正文色）。
- **详情**（`AnchorPopup`）：标题行（图标、标题）右侧是带文字的固定开关（`ChromeButton` `sm`，图标 `pin`，文字「固定」/「已固定」，固定时 `selected`）——单独一个图钉字形在右上角看起来像残影，不像开关；标题行下一条分隔线。本地 CPU、内存两行；远程 CPU、内存、网络、根文件系统，后两项在服务器报告后出现。打开期间宽度只增不减。弹层不取键盘焦点、Esc 不关闭——带焦点的弹层关闭时会把焦点还给终端，点击其他面板关闭弹层时焦点会被拽回。
- **固定**：外部点击不关闭，瓦片失焦后仍采样；不逐帧跟随锚点（会让窗口持续渲染），在每次采样与窗口尺寸变化时重新定位。固定后 `z` 为 `Theme.zBelow`，之后打开的弹层与菜单总在它上面——叠放顺序取决于弹层首次挂到 overlay 的先后，不是打开先后，先建的分支浮窗会被后建的资源浮窗压住。弹层关闭、读数收起或终端停止时取消固定。
- **与分支浮窗互斥**：打开详情前发 `detailOpening()`，宿主据此关闭分支浮窗；打开分支浮窗前宿主调用 `dismiss()`（取消固定并关闭详情）。见 [工作区组合 · 终端头部](../../ui/layout/workspace-composition.md#终端头部)。

## 3. 诊断

### `--resource-report`

Debug 构建传 `--resource-report=<文件.jsonl>`，`ResourceReporter`（`src/application/diagnostics/resource_reporter.*`）定期追加 CPU、GPU、内存、文件树节点数与窗口几何。

### 待机空转

`dev.bat diag idle-render` 自动完成（见 [构建 · 诊断探针](../../workflow/build.md#3-诊断探针dev-diag)）。手工方法：

```powershell
$env:QT_LOGGING_RULES = "qt.scenegraph.general=true;qt.scenegraph.time.renderloop=true;qt.quick.dirty=true"
.\target\build\SpanTui-test.exe --data-dir "$PWD\.dev\data"
# 待机 50 秒后结束，日志在 .dev\data\last-launch.log
```

- `syncAndRender: start` 行数 = 帧数；`updateDirtyNodes()` 行数 = GUI sync 次数，其后列出脏 Item。健康待机应长时间无帧。
- **帧数远多于 sync 且脏列表为空**：驱动源在渲染线程，即 `*Animator`。Animator 不受 Item 可见性约束，隐藏但 `running` 的 Animator 会让整窗空转；公共组件内的 Animator 须以有效可见性门控。
- 帧数与 sync 接近：按脏 Item 定位 GUI 侧的 `Behavior`/`Timer`/属性振荡。
- `QSG_VISUALIZE=changes` 在屏上闪显变化区域。

## 4. 代码与测试

- `src/monitoring/{resource_monitor, gpu_engine_metrics}.cpp`、`src/application/terminal_resource_targets.cpp`、`src/application/diagnostics/resource_reporter.cpp`、`qml/components/business/terminal/ResourceStrip.qml`
- 测试：`gpu_engine_metrics_test`、`resource_monitor_test`、`terminal_resource_targets_test`（真实 Shell 经生产采样器）
