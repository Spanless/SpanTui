import QtQuick
import SpanTui

// Expand/collapse affordance for explorer trees. Uses the filled
// tree-chevron glyph — the 24px stroke chevron vanishes at 16px.
Item {
    id: root

    property bool expanded: false
    property bool interactive: true
    property color color: Theme.muted

    signal toggled()

    readonly property int box: Theme.iconSm

    width: box
    height: box
    implicitWidth: box
    implicitHeight: box

    Icon {
        anchors.centerIn: parent
        name: "tree-chevron"
        size: root.box
        color: root.color
        rotation: root.expanded ? 90 : 0
        transformOrigin: Item.Center
    }

    MouseArea {
        anchors.fill: parent
        enabled: root.interactive
        hoverEnabled: true
        cursorShape: Qt.PointingHandCursor
        preventStealing: true
        onPressed: mouse => {
            mouse.accepted = true
            root.toggled()
        }
    }
}
