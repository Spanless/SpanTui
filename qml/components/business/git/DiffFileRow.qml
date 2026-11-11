pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Layouts
import SpanTui

// The list owns expansion and view mode.
ListItem {
    id: root

    property string path: ""
    property string oldPath: ""
    property string gitStatus: "modified"
    property int additions: 0
    property int deletions: 0
    property bool expanded: false
    property bool binary: false
    // Extra left inset that nests the row under its directory header.
    property int indent: 0
    property bool showDirectory: true
    // Width of the stat column, shared across the list so every row's name
    // area ends at the same x. The stat itself right-aligns inside it.
    property real statWidth: 0

    signal toggleRequested()

    readonly property string fileName: {
        const parts = path.split("/")
        return parts.length > 0 ? parts[parts.length - 1] : path
    }
    readonly property string directoryPath: {
        const slash = path.lastIndexOf("/")
        return slash > 0 ? path.substring(0, slash) : ""
    }
    readonly property bool renamed: oldPath.length > 0 && oldPath !== path
    // A move keeps the name, so the old location is the useful hint.
    readonly property string oldLabel: {
        if (!renamed)
            return ""
        const parts = oldPath.split("/")
        const oldName = parts[parts.length - 1]
        return oldName !== fileName ? oldName : oldPath
    }
    // The status reads from the glyph and name color (plus the strike-out of
    // a deleted name and the "← old" hint of a rename); rows carry no letter.
    readonly property color statusColor: {
        if (gitStatus === "deleted" || gitStatus === "conflict") return Theme.gitDeleted
        if (gitStatus === "added") return Theme.gitAdded
        if (gitStatus === "renamed" || gitStatus === "copied") return Theme.gitRenamed
        return Theme.gitModified
    }
    readonly property bool directoryShown: showDirectory && directoryPath.length > 0

    cursorShape: Qt.PointingHandCursor
    expandable: true
    onToggled: root.toggleRequested()

    // The directory gives way before the name. Natural widths come from
    // TextMetrics: an elided Text reports its elided width as implicit.
    TextMetrics { id: nameMetrics; font: nameText.font; text: nameText.text }
    TextMetrics { id: oldMetrics; font: oldText.font; text: oldText.text }

    RowLayout {
        anchors.fill: parent
        anchors.leftMargin: root.indent
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
            fileName: root.fileName
            size: Theme.iconMd
            color: root.statusColor
        }
        UiText {
            id: nameText
            readonly property real naturalWidth: Math.ceil(nameMetrics.advanceWidth) + 1
            Layout.fillWidth: true
            Layout.minimumWidth: 0
            Layout.preferredWidth: naturalWidth
            Layout.maximumWidth: naturalWidth
            text: root.fileName
            color: root.statusColor
            font.strikeout: root.gitStatus === "deleted"
            size: "md"
            elide: Text.ElideRight
        }
        UiText {
            id: oldText
            readonly property real naturalWidth: Math.ceil(oldMetrics.advanceWidth) + 1
            visible: root.renamed
            Layout.fillWidth: true
            Layout.minimumWidth: 0
            Layout.preferredWidth: naturalWidth
            Layout.maximumWidth: naturalWidth
            text: "← " + root.oldLabel
            tone: "muted"
            size: "sm"
            elide: Text.ElideMiddle
        }
        UiText {
            id: dirText
            visible: root.directoryShown
            Layout.fillWidth: true
            Layout.minimumWidth: 0
            Layout.preferredWidth: 0
            // A sliver of path is noise; fade it out without touching layout.
            opacity: width >= Theme.sizeLg * 2 ? 1 : 0
            text: root.directoryPath
            tone: "muted"
            size: "sm"
            // The tail names the directory; the head is shared context.
            elide: Text.ElideLeft
        }
        Item {
            // Takes the slack the directory would otherwise fill.
            visible: !root.directoryShown
            Layout.fillWidth: true
        }
        UiText {
            visible: root.binary
            text: qsTr("Binary")
            tone: "muted"
            size: "sm"
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

    // Built on hover only: a ToolTip popup per row is a large share of the
    // row's creation cost while a long list scrolls.
    Loader {
        anchors.fill: parent
        active: root.hovered && (nameText.truncated || oldText.truncated
                                 || (dirText.visible && dirText.truncated))
        sourceComponent: Item {
            Tip {
                visible: true
                text: root.renamed ? root.oldPath + " → " + root.path : root.path
            }
        }
    }
}
