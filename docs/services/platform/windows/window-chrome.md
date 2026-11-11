# 无边框窗口

`WindowChrome`（`src/platform/windows/window_chrome.*`）以原生事件过滤器处理：

| 消息 | 处理 |
|---|---|
| `WM_NCCALCSIZE` | 去掉系统标题栏与边框；最大化时裁掉外溢的 `SM_CXSIZEFRAME`（`window_chrome_geometry.cpp`） |
| `WM_NCHITTEST` | 标题拖拽区 `HTCAPTION`、八向调整区、其余 `HTCLIENT` |
| `WM_ENTERSIZEMOVE` / `WM_EXITSIZEMOVE` | 通知 `WindowTransitionCoordinator`（见 [视口事务](../../../ui/layout/viewport-transitions.md)） |

- **挂接顺序**：`attach()` 先安装过滤器，再补回 `WS_CAPTION` 等样式并 `SWP_FRAMECHANGED`。这次刷新会同步发 `WM_NCCALCSIZE`，过滤器未装好时系统会留出原生标题栏并截掉窗口底部，直到下一次尺寸变化。
- **深色窗框**（`setDarkFrame`）随 `Theme.dark` 设置 DWM 沉浸式深色：先用属性 20，失败再用 1809–1909 的 19，都不支持则保持浅色。
- **不启用 Snap Layouts**：窗口按钮全部是 `HTCLIENT`，`maxButtonRect` 恒为空——注册 `HTMAXBUTTON` 后系统会接管悬停与点击，QML 按钮的状态与动效失效。
- **任务栏自动隐藏**：最大化计算需要该状态，由后台低频检测缓存；`WM_NCCALCSIZE` 中只读原子缓存，不在消息循环里同步调用 Shell API。

视觉检查由维护者在边界画廊或真实窗口中进行（见 [像素边界](../../../ui/pixel-boundaries.md)）。
