import QtQuick
import QtQuick.Layouts
import SpanTui

// Shared disclosure header for grouped tree lists (Git refs, …).
// Toggle is owned by ListItem.expandable so rapid clicks are not dropped.
ListItem {
    id: root

    property string text: ""
    property string iconName: "folder"
    property bool expanded: true
    property int depth: 0

    expandable: true
    acceptedButtons: Qt.LeftButton
    cursorShape: Qt.PointingHandCursor

    RowLayout {
        anchors.fill: parent
        anchors.leftMargin: root.depth * Theme.treeDepthStep
        spacing: Theme.spaceSm
        z: Theme.zRaised

        TreeChevron {
            Layout.preferredWidth: Theme.iconSm
            Layout.preferredHeight: Theme.iconSm
            Layout.alignment: Qt.AlignVCenter
            expanded: root.expanded
            interactive: false
        }

        Icon {
            name: root.iconName
            size: Theme.iconMd
            color: Theme.muted
            Layout.alignment: Qt.AlignVCenter
        }

        UiText {
            Layout.fillWidth: true
            Layout.minimumWidth: 0
            text: root.text
            size: "md"
            elide: Text.ElideRight
        }
    }
}
