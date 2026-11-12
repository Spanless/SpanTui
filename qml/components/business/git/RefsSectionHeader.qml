import QtQuick
import QtQuick.Layouts
import SpanTui

// Top-level section header of the branch popup (local, other worktrees,
// remote, tags): a quiet caption, so the headers rank below the rows they
// introduce. Toggle is owned by ListItem.expandable.
ListItem {
    id: root

    property string text: ""
    property bool expanded: true

    expandable: true
    acceptedButtons: Qt.LeftButton
    cursorShape: Qt.PointingHandCursor
    rowHeight: Theme.sizeSm

    RowLayout {
        anchors.fill: parent
        spacing: Theme.spaceSm
        z: Theme.zRaised

        TreeChevron {
            Layout.preferredWidth: Theme.iconSm
            Layout.preferredHeight: Theme.iconSm
            Layout.alignment: Qt.AlignVCenter
            expanded: root.expanded
            interactive: false
        }

        UiText {
            Layout.fillWidth: true
            Layout.minimumWidth: 0
            text: root.text
            size: "sm"
            tone: "muted"
            emphasis: true
            elide: Text.ElideRight
        }
    }
}
