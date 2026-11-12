pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import SpanTui

// 所有浮层的公共根，业务不直接实例化。
// 子类收尾用 popupClosed()：它在出栈之后发出。
Popup {
    id: root

    property bool closeOnEscape: true
    property bool closeOnOutside: true
    // 自定义遮罩由子类直接覆写 Overlay.modal。
    property bool dimmed: false
    // AppDialog 的 enter/exit 是自己的覆写，不经此开关。
    property bool motionEnabled: true

    property Item anchorItem: null
    // left | right | center
    property string anchorAlign: "left"
    // 下方放不下时翻到上方。
    property bool anchorFlip: false
    property bool followAnchor: true
    // 自带尺寸绑定的子类（BranchPopup 等）关掉它，只用定位。
    property bool autoSizeToContent: true
    property int anchorGap: Theme.spaceSm
    property int edgeMargin: Theme.spaceMd
    property int maxHeightMargin: Theme.spaceLg

    readonly property real renderDpr: PixelGrid.safeDpr(Screen.devicePixelRatio)
    readonly property bool reduceMotion: Appearance.reduceMotion

    signal opening(var source)
    signal popupClosed()

    // `opening` runs before sizing and placement: its handlers reset the
    // content (filters, ratchets), and the popup is placed for that size.
    function openBelow(item) {
        if (!_beginOpen(item))
            return
        anchorItem = item
        opening(item)
        _applyAutoSize()
        _placeBelow()
        open()
    }

    function placeAt(item, localX, localY) {
        if (!_beginOpen(item))
            return
        anchorItem = null
        opening(item)
        _applyAutoSize()
        const point = item.mapToItem(Overlay.overlay, localX, localY)
        _clampTo(point.x, point.y)
        open()
    }

    // Gated on `visible`, not `opened`: `opened` turns true only after the
    // enter transition, and content laid out on the first visible frame
    // (implicit sizes settle in polish) would otherwise move the popup only
    // once that transition ended — a visible jump.
    function reposition() {
        if (!visible || !anchorItem)
            return
        _placeBelow()
    }

    function _beginOpen(item) {
        if (toggleGuard.shouldSkip(item))
            return false
        if (opened && toggleGuard.lastSource === item) {
            close()
            return false
        }
        if (!Overlay.overlay || !item)
            return false
        toggleGuard.remember(item)
        parent = Overlay.overlay
        return true
    }

    function _applyAutoSize() {
        if (!autoSizeToContent)
            return
        width = PixelGrid.snapSize(implicitWidth, renderDpr)
        height = Math.min(PixelGrid.snapSize(implicitHeight, renderDpr),
                          Overlay.overlay.height - maxHeightMargin)
    }

    function _placeBelow() {
        const overlay = Overlay.overlay
        const w = width
        const h = height
        var px
        if (anchorAlign === "right")
            px = anchorItem.mapToItem(overlay, anchorItem.width - w, 0).x
        else if (anchorAlign === "center")
            px = anchorItem.mapToItem(overlay, anchorItem.width / 2 - w / 2, 0).x
        else
            px = anchorItem.mapToItem(overlay, 0, 0).x
        var py = anchorItem.mapToItem(overlay, 0,
                                      anchorItem.height + anchorGap).y
        if (anchorFlip && py + h > overlay.height - edgeMargin) {
            const anchorTop = anchorItem.mapToItem(overlay, 0, 0)
            py = Math.max(edgeMargin, anchorTop.y - h - anchorGap)
        }
        _clampTo(px, py)
    }

    function _clampTo(px, py) {
        const overlay = Overlay.overlay
        x = Math.max(edgeMargin,
                     Math.min(px, overlay.width - width - edgeMargin))
        y = Math.max(edgeMargin,
                     Math.min(py, overlay.height - height - edgeMargin))
    }

    PopupToggleGuard { id: toggleGuard }

    modal: false
    focus: true
    // 每个带 CloseOnPressOutside 的弹层都会被同一次外点关闭，所以只有
    // PopupLayerStack 栈顶携带它。dimmer 收不到指针事件，不能在遮罩里拦截。
    closePolicy: {
        var flags = root.closeOnEscape ? Popup.CloseOnEscape
                                       : Popup.NoAutoClose
        if (root.closeOnOutside && PopupLayerStack.top === root)
            flags |= Popup.CloseOnPressOutside
        return flags
    }

    enter: PopupEnter { enabled: root.motionEnabled && !root.reduceMotion }
    exit: PopupExit { enabled: root.motionEnabled && !root.reduceMotion }

    Overlay.modal: Rectangle {
        color: root.dimmed ? Theme.scrim : "transparent"
        Behavior on opacity {
            enabled: !root.reduceMotion
            NumberAnimation { duration: Theme.durMid }
        }
    }

    background: PopupSurface {
        devicePixelRatio: root.renderDpr
    }

    onOpened: PopupLayerStack.push(root)
    onAboutToHide: toggleGuard.arm()
    onClosed: {
        PopupLayerStack.remove(root)
        root.popupClosed()
    }
    // Destruction while open skips `closed` on some paths — a dead entry
    // left on top would strip CloseOnPressOutside from every lower popup.
    Component.onDestruction: PopupLayerStack.remove(root)
    onWidthChanged: if (followAnchor)
                        reposition()
    onHeightChanged: if (followAnchor)
                         reposition()

    // Ancestors move the anchor (window resize, open transitions) without
    // any geometry signal reaching the popup, so it is re-placed per frame.
    FrameAnimation {
        running: root.visible && root.followAnchor && root.anchorItem !== null
        onTriggered: root.reposition()
    }
}
