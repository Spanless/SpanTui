import QtQuick
import SpanTui

Item {
    id: root

    property string text: ""
    property string iconName: "help"
    property int iconSize: Theme.iconSm
    property real maxTooltipWidth: 300

    implicitWidth: Math.max(Theme.iconSm + 8, 22)
    implicitHeight: Math.max(Theme.iconSm + 8, 22)
    visible: text.length > 0

    readonly property real renderDpr: PixelGrid.safeDpr(Screen.devicePixelRatio)
    readonly property bool hovered: hoverArea.containsMouse

    CrispSurface {
        anchors.fill: parent
        devicePixelRatio: root.renderDpr
        radius: width / 2
        fillEnabled: root.hovered
        fillTopColor: Theme.hover
        fillMiddleColor: fillTopColor
        fillBottomColor: fillTopColor
        strokeEnabled: false
    }

    Icon {
        anchors.centerIn: parent
        name: root.iconName
        size: root.iconSize
        color: root.hovered ? Theme.text : Theme.muted
        opacity: root.hovered ? 1 : Theme.fadeMd
    }

    MouseArea {
        id: hoverArea
        anchors.fill: parent
        hoverEnabled: true
        cursorShape: Qt.PointingHandCursor
        acceptedButtons: Qt.NoButton
    }

    Tip {
        text: root.text
        visible: root.text.length > 0 && root.hovered
        delay: Theme.tipDelayQuick
        wrap: true
        maxTextWidth: root.maxTooltipWidth
        minTextWidth: 120      // style-ok: readable floor for wrapped hints
        textVPad: Theme.spaceMd
        textLineHeight: 1.2
    }
}
