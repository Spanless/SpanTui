import QtQuick
import SpanTui

// Shared toggle switch on Theme tokens — the app's only switch chrome.
//
// Contract: `checked` is owned by the caller (bind it to your model). User
// interaction emits toggled(newChecked) as a *request* — the caller updates
// its model and the binding flows back. Never assign to `checked` inside.
Item {
    id: root

    property bool checked: false
    property string text: ""
    property string tip: ""

    signal toggled(bool newChecked)

    readonly property real renderDpr: PixelGrid.safeDpr(Screen.devicePixelRatio)
    readonly property bool reduceMotion: Appearance.reduceMotion
    readonly property int trackW: PixelGrid.snapSize(36, renderDpr)   // style-ok: switch track length
    readonly property int trackH: PixelGrid.snapSize(20, renderDpr)   // style-ok: switch track height
    readonly property int handleInset: Theme.thumbInset
    readonly property int handleD: trackH - handleInset * 2

    // Pointer engagement only — keyboard focus shows the ring, never a fill.
    readonly property bool engaged: enabled && hoverArea.containsMouse
    readonly property string trackState: !root.enabled ? ""
                                         : hoverArea.pressed ? "pressed"
                                         : root.engaged ? "hover" : ""

    implicitWidth: row.implicitWidth
    implicitHeight: Theme.sizeSm
    width: implicitWidth
    height: implicitHeight

    activeFocusOnTab: true
    opacity: enabled ? 1 : Theme.opacityDisabled

    Accessible.role: Accessible.CheckBox
    Accessible.name: root.text.length > 0 ? root.text : qsTr("Toggle")
    Accessible.onPressAction: root.toggled(!root.checked)

    Keys.onSpacePressed: root.toggled(!root.checked)

    Row {
        id: row
        anchors.verticalCenter: parent.verticalCenter
        spacing: Theme.spaceMd

        Item {
            width: root.trackW
            height: root.trackH
            anchors.verticalCenter: parent.verticalCenter

            CrispSurface {
                anchors.fill: parent
                devicePixelRatio: root.renderDpr
                radius: height / 2
                // Rimless track: solid accent when on (solidFill recipe),
                // switchTrackOff when off (scheme state layers on top).
                fillTopColor: root.checked
                              ? Theme.solidFill(Theme.accent, root.trackState)
                              : (root.trackState.length > 0
                                 ? Theme.stateFill(Theme.switchTrackOff, root.trackState)
                                 : Theme.switchTrackOff)
                fillMiddleColor: fillTopColor
                fillBottomColor: fillTopColor
                focusRing: root.activeFocus
            }

            CrispSurface {
                id: handle
                width: root.handleD
                height: root.handleD
                y: root.handleInset
                x: root.checked ? root.trackW - width - root.handleInset
                                : root.handleInset
                devicePixelRatio: root.renderDpr
                radius: width / 2
                elevation: "raised"

                Behavior on x {
                    enabled: !root.reduceMotion
                    NumberAnimation { duration: Theme.durFast; easing.type: Easing.OutCubic }
                }
            }
        }

        UiText {
            visible: root.text.length > 0
            text: root.text
            size: "md"
            anchors.verticalCenter: parent.verticalCenter
            color: root.enabled ? Theme.text : Theme.muted
        }
    }

    MouseArea {
        id: hoverArea
        anchors.fill: parent
        hoverEnabled: true
        cursorShape: Qt.PointingHandCursor
        onClicked: root.toggled(!root.checked)
    }

    Tip {
        text: root.tip
        visible: root.tip.length > 0 && hoverArea.containsMouse
    }
}
