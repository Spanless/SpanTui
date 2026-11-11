import QtQuick
import SpanTui

// Shared pane empty state: centered muted icon, lg title, sm subtitle and an
// optional action. `active` owns both visibility and the entrance; the block
// itself rides at fadeLg so it never competes with real content. Entrance is
// staggered top-down: each block rises a small step and fades in on its own
// eased slice of one clock, and the icon settles from a slight shrink.
// Reduced motion snaps.
Item {
    id: root

    property bool active: true
    property string icon: ""
    property color iconColor: Theme.muted
    property string title: ""
    property string subtitle: ""
    property string actionText: ""
    property string actionIcon: ""

    signal actionTriggered()

    // Entrance clock (0 → 1 over durSlow). Block i starts at i * staggerShift
    // and takes staggerSpan of the clock, eased out-cubic.
    property real entrance: 1
    readonly property real staggerShift: 0.15
    readonly property real staggerSpan: 0.55
    readonly property real iconRestScale: 0.88

    function blockEnter(i) {
        const t = Math.max(0, Math.min(1, (entrance - i * staggerShift) / staggerSpan))
        return 1 - Math.pow(1 - t, 3)
    }

    function _playEntrance() {
        if (!active || Appearance.reduceMotion) {
            entranceAnim.stop()
            entrance = 1
            return
        }
        entranceAnim.restart()
    }

    onActiveChanged: _playEntrance()
    Component.onCompleted: _playEntrance()

    NumberAnimation {
        id: entranceAnim
        target: root
        property: "entrance"
        from: 0
        to: 1
        duration: Theme.durSlow
    }

    visible: active
    opacity: active ? Theme.fadeLg : 0
    Behavior on opacity {
        enabled: !Appearance.reduceMotion
        NumberAnimation { duration: Theme.durMid; easing.type: Easing.OutCubic }
    }

    // Hosts that size it to its content (instead of filling a pane) get the
    // block plus spaceLg above and below.
    implicitHeight: content.implicitHeight + Theme.spaceLg * 2

    Column {
        id: content
        anchors.centerIn: parent
        width: Math.min(parent.width - Theme.spaceLg * 2, Theme.emptyContentW)
        spacing: Theme.spaceMd

        Icon {
            anchors.horizontalCenter: parent.horizontalCenter
            visible: root.icon.length > 0
            name: root.icon
            size: Theme.iconLg
            color: root.iconColor
            opacity: Theme.fadeMd * root.blockEnter(0)
            scale: root.iconRestScale + (1 - root.iconRestScale) * root.blockEnter(0)
            transform: Translate { y: (1 - root.blockEnter(0)) * Theme.spaceSm }
        }

        UiText {
            width: parent.width
            visible: root.title.length > 0
            horizontalAlignment: Text.AlignHCenter
            text: root.title
            size: "lg"
            wrapMode: Text.WordWrap
            opacity: root.blockEnter(1)
            transform: Translate { y: (1 - root.blockEnter(1)) * Theme.spaceSm }
        }

        UiText {
            width: parent.width
            visible: root.subtitle.length > 0
            horizontalAlignment: Text.AlignHCenter
            text: root.subtitle
            size: "sm"
            tone: "muted"
            wrapMode: Text.WordWrap
            opacity: root.blockEnter(2)
            transform: Translate { y: (1 - root.blockEnter(2)) * Theme.spaceSm }
        }

        // The wrapper carries the entrance fade — ChromeButton's own opacity
        // is its disabled tier.
        Item {
            anchors.horizontalCenter: parent.horizontalCenter
            visible: root.actionText.length > 0
            width: actionButton.width
            height: actionButton.height
            opacity: root.blockEnter(3)
            transform: Translate { y: (1 - root.blockEnter(3)) * Theme.spaceSm }

            ChromeButton {
                id: actionButton
                iconName: root.actionIcon
                text: root.actionText
                onClicked: root.actionTriggered()
            }
        }
    }
}
