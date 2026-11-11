import QtQuick
import SpanTui

// Compact "+adds -dels" stat pair for git surfaces. Each side hides when its
// count is zero; mono digits keep the pair stable as values change.
Row {
    id: root

    property int additions: 0
    property int deletions: 0
    property int px: Theme.uiSm

    spacing: Theme.spaceSm

    MonoText {
        anchors.verticalCenter: parent.verticalCenter
        visible: root.additions > 0
        text: "+" + root.additions
        color: Theme.gitAdded
        px: root.px
    }
    MonoText {
        anchors.verticalCenter: parent.verticalCenter
        visible: root.deletions > 0
        text: "-" + root.deletions
        color: Theme.gitDeleted
        px: root.px
    }
}
