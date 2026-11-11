import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import SpanTui

// Shared modal dialog shell: centered PopupSurface card over the theme scrim,
// lg heading + muted message, caller body, right-aligned confirm row. All
// dialogs share the slide+pop enter/exit recipe and the dialog size ladder.
AppPopup {
    id: root

    property string heading: ""
    property string message: ""
    // "sm" | "md" | "lg" — width tier; md is the default prompt.
    property string size: "md"
    // Two-physical-pixel accent signature along the top edge (name/identity
    // prompts); plain confirms leave it off.
    property bool accentRim: false
    property string cancelText: qsTr("Cancel")
    property string confirmText: ""
    // "primary" | "danger" — the confirm button's kind.
    property string confirmKind: "primary"
    property bool confirmEnabled: true
    default property alias body: bodySlot.data

    signal confirmed()
    signal cancelled()

    readonly property int dialogW: size === "sm" ? Theme.dialogSm
                                 : size === "lg" ? Theme.dialogLg
                                 : Theme.dialogMd
    property real slide: 0
    property real pop: 1

    anchors.centerIn: Overlay.overlay
    width: Math.min(dialogW, Overlay.overlay
                     ? Overlay.overlay.width - Theme.spaceLg * 2 : dialogW)
    padding: Theme.spaceLg
    modal: true
    dimmed: true
    closeOnOutside: false
    scale: pop

    enter: Transition {
        enabled: !root.reduceMotion
        ParallelAnimation {
            NumberAnimation { property: "opacity"; from: 0; to: 1; duration: Theme.durMid; easing.type: Easing.OutCubic }
            NumberAnimation { property: "slide"; from: Theme.spaceLg; to: 0; duration: Theme.durSlow; easing.type: Easing.OutQuint }
            NumberAnimation { property: "pop"; from: 0.94; to: 1; duration: Theme.durMid; easing.type: Easing.OutBack; easing.overshoot: 1.1 }
        }
    }
    exit: Transition {
        enabled: !root.reduceMotion
        ParallelAnimation {
            NumberAnimation { property: "opacity"; to: 0; duration: Theme.durFast; easing.type: Easing.OutCubic }
            NumberAnimation { property: "slide"; to: Theme.spaceMd; duration: Theme.durFast; easing.type: Easing.OutCubic }
            NumberAnimation { property: "pop"; to: 0.97; duration: Theme.durFast; easing.type: Easing.OutCubic }
        }
    }

    background: Item {
        transform: Translate { y: root.slide }
        PopupSurface {
            anchors.fill: parent
            radius: Theme.radiusXl
            devicePixelRatio: root.renderDpr
        }
        PixelLine {
            visible: root.accentRim
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.top: parent.top
            anchors.leftMargin: Theme.radiusXl
            anchors.rightMargin: Theme.radiusXl
            height: implicitHeight
            thicknessPx: 2
            lineColor: Theme.accent
            opacity: Theme.fadeLg
        }
    }

    contentItem: ColumnLayout {
        spacing: Theme.spaceLg
        transform: Translate { y: root.slide }

        ColumnLayout {
            Layout.fillWidth: true
            spacing: Theme.spaceMd
            visible: root.heading.length > 0 || root.message.length > 0

            UiText {
                visible: root.heading.length > 0
                text: root.heading
                size: "lg"
            }
            UiText {
                Layout.fillWidth: true
                visible: root.message.length > 0
                text: root.message
                wrapMode: Text.WordWrap
            }
        }

        ColumnLayout {
            id: bodySlot
            Layout.fillWidth: true
        }

        RowLayout {
            Layout.fillWidth: true
            spacing: Theme.spaceMd
            visible: root.confirmText.length > 0 || root.cancelText.length > 0

            Item { Layout.fillWidth: true }
            ChromeButton {
                visible: root.cancelText.length > 0
                kind: "secondary"
                text: root.cancelText
                onClicked: root.cancelled()
            }
            ChromeButton {
                visible: root.confirmText.length > 0
                kind: root.confirmKind
                text: root.confirmText
                enabled: root.confirmEnabled
                onClicked: root.confirmed()
            }
        }
    }
}
