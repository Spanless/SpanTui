import QtQuick
import SpanTui

// L2 裸锚定面板：AppPopup 的定位/表面/守卫 + 一个默认槽。
// 用于弹体塞不进 AnchorPopup "标题行+列表" 配方的场景
// （双栏、自定义编辑器等）。槽内子项必须自带尺寸
// （Column/Row/Positioner 或有显式宽高的 Item）。
AppPopup {
    id: root

    default property alias panelBody: bodySlot.data

    padding: Theme.spaceMd

    contentItem: Item {
        id: bodySlot
        implicitWidth: childrenRect.width
        implicitHeight: childrenRect.height
    }
}
