# 无障碍与动效

## 1. 键盘与焦点

- Tab 顺序与视觉顺序一致。
- 模态对话框把焦点留在框内；浮层与弹窗支持 Esc 关闭。
- 图标按钮有 tooltip 与无障碍文本。

## 2. 减弱动效

`Appearance.reduceMotion` 跟随 Windows"显示动画"开关，新动画写 `enabled: !Appearance.reduceMotion`。开启时：

- 布局、侧栏折叠、颜色过渡、按钮缩放、错峰入场、树展开直切；
- 图标不播放悬浮与按下，激活瞬切；加载图标静止；
- 终端光标由 1060ms 呼吸改为 530ms 离散闪烁。

## 3. 对比度

- 状态不只靠颜色表达，同时给文字或形状。
- 文字色经 `Theme.readable()` 保底 WCAG AA。
- 无描边控件靠底色差区分；焦点一律经 `CrispSurface.focusRing` 可见。
- accent 底上的文字与图标用 `Theme.accentText`（按 accent 亮度取近黑或白）。
