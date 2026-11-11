import QtQuick
import QtQuick.Controls
import SpanTui

ToolTip {
    id: control

    readonly property real renderDpr: PixelGrid.safeDpr(Screen.devicePixelRatio)

    // Multi-line hints: wrap plus width clamp; single-line tips stay implicit-sized.
    property bool wrap: false
    property real maxTextWidth: 0    // 0 = unbounded
    property real minTextWidth: 0
    property int textVPad: Theme.spaceSm
    property real textLineHeight: 1.0

    x: parent ? (parent.width - width) / 2 : 0
    y: parent ? parent.height + 6 : 0

    delay: Theme.tipDelay
    timeout: Theme.tipTimeout
    margins: Theme.spaceMd
    padding: 0

    contentItem: UiText {
        text: control.text
        size: "sm"
        wrapMode: control.wrap ? Text.WordWrap : Text.NoWrap
        width: control.wrap
               ? Math.min(control.maxTextWidth > 0 ? control.maxTextWidth
                                                  : implicitWidth,
                          Math.max(control.minTextWidth, implicitWidth))
               : implicitWidth
        lineHeight: control.textLineHeight
        leftPadding: Theme.spaceMd
        rightPadding: Theme.spaceMd
        topPadding: control.textVPad
        bottomPadding: control.textVPad
    }

    // One text line tall: the popup radiusLg would round it into a pill.
    background: PopupSurface {
        devicePixelRatio: control.renderDpr
        radius: Theme.radiusSm
    }

    enter: PopupEnter {}
    exit: PopupExit {}
}
