import QtQuick
import QtQuick.Layouts
import SpanTui

// Directory header inside a grouped diff list: groups the file rows below it
// and shows their summed stat. Presentation only; the list owns open state.
ListItem {
    id: root

    // Repository-relative directory; empty for files at the root.
    property string path: ""
    property int additions: 0
    property int deletions: 0
    property bool expanded: true
    // Shared stat column width; see DiffFileRow.statWidth.
    property real statWidth: 0

    signal toggleRequested()

    cursorShape: Qt.PointingHandCursor
    expandable: true
    onToggled: root.toggleRequested()

    // Cap from TextMetrics: an elided Text reports its elided width as
    // implicit, so an implicitWidth cap would ratchet down for good.
    TextMetrics { id: pathMetrics; font: pathText.font; text: pathText.text }

    RowLayout {
        anchors.fill: parent
        spacing: root.contentGap
        z: Theme.zRaised

        TreeChevron {
            Layout.preferredWidth: Theme.iconSm
            Layout.minimumWidth: Theme.iconSm
            Layout.preferredHeight: Theme.iconSm
            expanded: root.expanded
            interactive: false
        }
        FileIcon {
            isDir: true
            expanded: root.expanded
            size: Theme.iconMd
            color: Theme.muted
        }
        UiText {
            id: pathText
            Layout.fillWidth: true
            Layout.minimumWidth: 0
            Layout.preferredWidth: Math.ceil(pathMetrics.advanceWidth) + 1
            Layout.maximumWidth: Math.ceil(pathMetrics.advanceWidth) + 1
            text: root.path.length > 0 ? root.path : qsTr("Repository root")
            size: "md"
            // The tail names the directory; the head is shared context.
            elide: Text.ElideLeft
        }
        Item {
            Layout.fillWidth: true
        }
        Item {
            readonly property real columnWidth: Math.max(root.statWidth, stat.implicitWidth)
            Layout.preferredWidth: columnWidth
            Layout.minimumWidth: columnWidth
            Layout.fillHeight: true
            DiffStat {
                id: stat
                anchors.right: parent.right
                anchors.verticalCenter: parent.verticalCenter
                additions: root.additions
                deletions: root.deletions
            }
        }
    }

    Tip {
        visible: root.hovered && pathText.truncated
        text: root.path
    }
}
