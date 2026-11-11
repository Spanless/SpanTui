# 像素边界

非整数缩放（125%、150%）下，逻辑 1px 线会跨物理像素被抗锯齿成灰带。对齐原语：

| 原语 | 源码 | 作用 |
|---|---|---|
| `PixelGrid` | `qml/PixelGrid.qml` | 单例：`snap(value)`、`devicePixelRatio`、`pixel` |
| `PixelLine` | `qml/components/common/PixelLine.qml` | 单物理像素线 |
| `PixelSnap` | `src/application/pixel_snap.cpp` | `Icon` 变换列表末项：每帧在 `afterAnimating` 把字形原点取整到物理像素，祖先移动时重算；祖先含旋转时不对齐 |
| `TerminalCellMaterial` | `src/terminal/render/terminal_cell_material.cpp`、`shaders/terminal_cell.*` | 基线与格子坐标取整到物理像素；特殊字形按设备像素光栅化并采样纹素中心（见 [终端渲染](../services/terminal/rendering-scrollback.md#1-场景图渲染与快照交换)） |
| `CrispSurface` | `qml/components/common/CrispSurface.qml` | 圆角、描边、投影；直边吸附物理像素，圆弧 2×2 子像素覆盖率，填充与描边互斥合成（避免焦点框圆角外缘重复变暗） |

规则：

- 相邻面板接缝只由一侧画线。
- 瓦片内侧直角贴合，外圆角由最外层容器负责。

验证：`--boundary-gallery-theme=<mode>` 或 `--boundary-gallery-shot=<path>` 打开 `qml/BoundaryGallery.qml`，由维护者在 100%–200% 五档缩放下人工查看；高对比焦点框的圆角外缘不应比直边更深。测试：`tests/application/pixel_snap_test.cpp`。
