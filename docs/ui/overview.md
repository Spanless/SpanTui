# UI 层总览

`qml/` 的边界与通用约束。

## 1. 边界

QML 只做渲染、动画、焦点与交互；只持展示态（焦点、弹层可见性）。不读写数据库、文件或进程，不持领域数据副本，不实现跨域逻辑；跨域操作经 `WorkspaceController` 或领域服务接口。

```
qml/
├── Main.qml              工作区组合根
├── Settings.qml          设置浮层
├── TerminalPanel.qml     独立终端根
├── BoundaryGallery.qml   边界画廊夹具
├── Theme.qml             设计 token 单例
├── PixelGrid.qml         像素对齐单例
├── ScrollPolicy.qml      滚动策略单例
└── components/{base,common,business}/
```

## 2. 通用约束

- 文案用 `qsTr()`（见 [国际化](../workflow/localization.md)）。
- 视觉值取 `Theme` token，`tools/check-style.ps1` 拦截裸数值，特例写 `// style-ok: 原因`（见 [设计系统](design-system.md)）。
- 先查 [组件库](components/overview.md)，能复用就不重写；浮层一律走 `AppPopup` 体系。
- common/base 只经 `Appearance` 单例读偏好；business 还可绑定 `TerminalSettings`、`WorkspaceSettings` 单例。
- 滚动视口归父容器；标签与控件对齐用 `GridRow`/`GridCol`。
- 发丝线用 `PixelLine`，接缝只一侧绘制（见 [像素边界](pixel-boundaries.md)）。
- 新动画响应减弱动效（见 [无障碍与动效](accessibility-motion.md)）。
- 写成编译器能编译的形式：含委托、`Loader` 内联组件或 `Component` 的文件加 `pragma ComponentBehavior: Bound`，委托用 `required property` 取 `index`/`modelData`/角色，嵌套子项经委托 id 访问它们；外层属性经 `root.` 访问；C++ 对象属性按类型声明（如 `GitRepository`），不用 `var`；函数参数与返回值写类型注解；单例直接引用，不经属性转手（见 [架构总览](../architecture/overview.md)）。

## 3. 子域

| 子域 | 文档 |
|---|---|
| 设计 token、状态色、尺寸阶梯 | [design-system.md](design-system.md) |
| 共享组件 | [components/overview.md](components/overview.md) |
| 工作区布局 | [layout/workspace-composition.md](layout/workspace-composition.md) |
| 视口事务 | [layout/viewport-transitions.md](layout/viewport-transitions.md) |
| 像素边界 | [pixel-boundaries.md](pixel-boundaries.md) |
| 图标 | [icons.md](icons.md) |
| 无障碍与动效 | [accessibility-motion.md](accessibility-motion.md) |

## 4. 本域改动完成标准

- [ ] 新文案已 `qsTr()` 并补齐 `.ts` 译文，`tools/check-translations.ps1` 通过。
- [ ] `tools/check-style.ps1`、`tools/check-icons.ps1` 通过。
- [ ] 未重写已有组件能力；第二个相似用法已抽到 common。
- [ ] QML 未缓存领域数据，跨域交互未越层。
- [ ] smoke 输出 `SMOKE_OK`，启动日志无新 QML 警告。
- [ ] Tab 顺序与视觉一致，新弹窗可 Esc 关闭，图标按钮有无障碍文本，新动画受减弱动效控制。
- [ ] 交付说明注明未做视觉验收及需查看的界面（DPI 边界、发丝线、圆角）。
