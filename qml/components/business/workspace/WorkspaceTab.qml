pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Layouts
import SpanTui

// Shared workspace Tab for terminal sessions, documents and utility views.
// Compact capsule, vertically centered in the header strip. The active Tab
// is a raised thumb with a Medium label (the segmented-control selection
// rule); inactive Tabs are flat and only tint on hover / press. Keyboard
// focus adds the shared focus ring.
Item {
    id: root

    property string text: ""
    property string iconName: ""
    property string fileName: ""
    property bool useFileIcon: false
    property color iconColor: Theme.muted
    property color textColor: Theme.text
    property bool active: false
    property bool statusVisible: false
    property color statusColor: Theme.accent
    property bool closeEnabled: true
    property bool middleClickCloses: true
    property string closeTip: qsTr("Close")

    signal activated()
    signal closeRequested()
    signal contextMenuRequested(var source, real x, real y)

    readonly property real renderDpr: PixelGrid.safeDpr(Screen.devicePixelRatio)
    readonly property real chipHeight: PixelGrid.snapSize(Theme.sizeSm, renderDpr)
    readonly property real chipInset: PixelGrid.snapSize(Theme.spaceSm, renderDpr)
    readonly property real chipY: PixelGrid.snapSize(
        Math.max(0, (height - chipHeight) / 2), renderDpr)
    readonly property real contentInset: PixelGrid.snapSize(Theme.spaceMd, renderDpr)
    readonly property real closeSlot: PixelGrid.snapSize(Theme.sizeSm, renderDpr)
    readonly property bool hovered: hoverHandler.hovered
    // Flat-state tint of an inactive Tab; the active Tab takes the raised
    // recipe instead.
    readonly property color capsuleFill: tabMouse.pressed ? Theme.pressed
                                         : root.hovered ? Theme.hover
                                         : "transparent"

    width: PixelGrid.snapSize(Theme.sizeLg * 5, renderDpr)
    activeFocusOnTab: true
    z: active ? 1 : 0

    Keys.onReturnPressed: root.activated()
    Keys.onEnterPressed: root.activated()
    Keys.onSpacePressed: root.activated()

    HoverHandler { id: hoverHandler }

    CrispSurface {
        x: root.chipInset
        y: root.chipY
        width: Math.max(0, root.width - root.chipInset * 2)
        height: root.chipHeight
        devicePixelRatio: root.renderDpr
        radius: Theme.radiusSm
        elevation: root.active ? "raised" : "none"
        fillEnabled: root.active || root.capsuleFill.a > 0
        fillTopColor: root.active ? Theme.raisedFillTop : root.capsuleFill
        fillMiddleColor: root.active ? Theme.raisedFill : root.capsuleFill
        focusRing: root.activeFocus
    }

    RowLayout {
        z: Theme.zRaised
        x: root.chipInset + root.contentInset
        y: root.chipY
        width: Math.max(0, root.width - (root.chipInset + root.contentInset) * 2)
        height: root.chipHeight
        spacing: Theme.spaceSm
        clip: true

        Loader {
            visible: root.useFileIcon || root.iconName.length > 0
            Layout.alignment: Qt.AlignVCenter
            Layout.preferredWidth: Theme.iconSm
            Layout.preferredHeight: Theme.iconSm
            sourceComponent: root.useFileIcon ? fileIconComponent : vectorIconComponent
        }
        Component {
            id: fileIconComponent
            FileIcon {
                fileName: root.fileName
                isDir: false
                size: Theme.iconSm
                color: root.active || root.hovered ? root.textColor : Theme.muted
            }
        }
        Component {
            id: vectorIconComponent
            Icon { name: root.iconName; size: Theme.iconSm; color: root.iconColor }
        }

        UiText {
            Layout.fillWidth: true
            Layout.minimumWidth: 0
            Layout.alignment: Qt.AlignVCenter
            size: "md"
            emphasis: root.active
            color: root.active || root.hovered || tabMouse.pressed ? root.textColor
                                                                  : Theme.muted
            text: root.text
            horizontalAlignment: Text.AlignLeft
            verticalAlignment: Text.AlignVCenter
            elide: Text.ElideMiddle
            maximumLineCount: 1
            wrapMode: Text.NoWrap
        }

        CrispSurface {
            visible: root.statusVisible
            Layout.preferredWidth: PixelGrid.snapSize(6, root.renderDpr)
            Layout.preferredHeight: PixelGrid.snapSize(6, root.renderDpr)
            devicePixelRatio: root.renderDpr
            radius: height / 2
            fillTopColor: root.statusColor
            fillMiddleColor: fillTopColor
            fillBottomColor: fillTopColor
            strokeEnabled: false
        }

        ChromeButton {
            Layout.alignment: Qt.AlignVCenter
            Layout.preferredWidth: root.closeSlot
            Layout.preferredHeight: root.closeSlot
            Layout.maximumWidth: root.closeSlot
            Layout.maximumHeight: root.closeSlot
            size: "sm"
            kind: "ghost"
            iconName: "x"
            tip: root.closeTip
            opacity: root.closeEnabled && (root.hovered || root.active || root.activeFocus) ? 1 : 0
            enabled: root.closeEnabled && (root.hovered || root.active || root.activeFocus)
            onClicked: root.closeRequested()
        }
    }

    MouseArea {
        id: tabMouse
        // Hit layer: above the capsule face, under the content row.
        z: Theme.zRaised - 1
        anchors.fill: parent
        hoverEnabled: true
        acceptedButtons: Qt.LeftButton | Qt.MiddleButton | Qt.RightButton
        cursorShape: Qt.PointingHandCursor
        onClicked: mouse => {
            if (mouse.button === Qt.MiddleButton && root.middleClickCloses) {
                root.closeRequested()
                return
            }
            if (mouse.button === Qt.RightButton) {
                root.contextMenuRequested(root, mouse.x, mouse.y)
                return
            }
            root.activated()
        }
    }
}
