import QtQuick
import QtQuick.Layouts
import SpanTui

// Inline status strip for pane bodies: tinted surface + optional leading
// StatusIcon (kind from `tone`) + text + optional action and dismiss buttons. `active` owns an animated sizeMd collapse;
// `inset` floats the strip one gutter step deeper as a card, otherwise it runs
// flush to the pane edge.
CrispSurface {
    id: root

    property bool active: false
    property string tone: "danger"      // danger | warning | info
    property string text: ""
    property bool showIcon: true
    property string textSize: "md"
    property bool inset: false
    property bool dismissible: false
    // Non-empty shows a trailing push button that emits actionTriggered().
    property string actionText: ""

    signal dismissed()
    signal actionTriggered()

    readonly property color fillColor: tone === "warning"
        ? Theme.warningSoft
        : tone === "info" ? Theme.accentSoft : Theme.dangerSoft
    readonly property string textTone: tone === "info" ? "accent" : tone
    readonly property bool reduceMotion: Appearance.reduceMotion

    implicitHeight: active ? Theme.sizeMd : 0
    clip: true
    visible: opacity > 0
    opacity: active ? 1 : 0
    radius: inset ? Theme.radiusMd : 0
    devicePixelRatio: PixelGrid.safeDpr(Screen.devicePixelRatio)
    fillTopColor: fillColor
    fillMiddleColor: fillColor
    fillBottomColor: fillColor
    strokeEnabled: false
    Layout.leftMargin: inset ? Theme.spaceMd : 0
    Layout.rightMargin: inset ? Theme.spaceMd : 0

    Behavior on implicitHeight {
        enabled: !root.reduceMotion
        NumberAnimation { duration: Theme.durMid; easing.type: Easing.OutCubic }
    }
    Behavior on opacity {
        enabled: !root.reduceMotion
        NumberAnimation { duration: Theme.durMid }
    }

    RowLayout {
        anchors.fill: parent
        anchors.leftMargin: Theme.spaceMd
        anchors.rightMargin: Theme.spaceSm
        spacing: Theme.spaceMd

        StatusIcon {
            visible: root.showIcon
            kind: root.tone === "danger" ? "error" : root.tone
            size: Theme.iconSm
        }
        UiText {
            Layout.fillWidth: true
            text: root.text
            size: root.textSize
            tone: root.textTone
            elide: Text.ElideRight
        }
        ChromeButton {
            visible: root.actionText.length > 0
            text: root.actionText
            size: "sm"
            onClicked: root.actionTriggered()
        }
        ChromeButton {
            visible: root.dismissible
            kind: "ghost"
            iconName: "x"
            size: "sm"
            tip: qsTr("Dismiss")
            onClicked: root.dismissed()
        }
    }
}
