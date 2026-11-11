import QtQuick
import QtQuick.Layouts
import SpanTui

// Shared file-tree row used by the workspace tree. Git status is presentation
// only; the owning view keeps navigation.
ListItem {
    id: root

    property string fileName: ""
    property bool directory: false
    property bool expanded: false
    property bool revealed: true
    property bool animateReveal: false
    property bool revealReady: false
    // ListView recycles delegates; skip height animation across row identity.
    property int recycleKey: 0
    property bool loading: false
    property bool rootDirectory: false
    property int depth: 0
    property string gitStatus: ""
    property string aggregateGitStatus: ""
    property bool contentVisible: true
    property bool statusVisible: true
    property string statusText: ""

    readonly property string effectiveGitStatus:
        directory ? aggregateGitStatus : gitStatus

    function gitColor(status) {
        if (status === "ignored") return Theme.gitIgnored
        if (status === "conflict" || status === "deleted") return Theme.gitDeleted
        if (status === "added") return Theme.gitAdded
        if (status === "renamed" || status === "copied") return Theme.gitRenamed
        if (String(status || "").length > 0) return Theme.gitModified
        return Theme.text
    }

    function gitLabel(status) {
        if (status === "ignored") return ""
        if (status === "conflict") return "!"
        if (status === "added") return "A"
        if (status === "deleted") return "D"
        if (status === "renamed") return "R"
        if (status === "copied") return "C"
        return String(status || "").length > 0 ? "M" : ""
    }

    expandable: directory
    // The chevron below owns the toggle; single clicks stay selection.
    toggleOnPress: false
    height: revealed ? rowHeight : 0
    implicitHeight: height
    clip: true
    enabled: height > 1
    opacity: height > 0.5 ? 1 : 0 // style-ok: half-grown collapse threshold

    Component.onCompleted: revealReady = true
    onRecycleKeyChanged: {
        revealReady = false
        Qt.callLater(function() { root.revealReady = true })
    }

    Behavior on height {
        enabled: root.animateReveal && root.revealReady
        NumberAnimation {
            duration: Theme.durFast
            easing.type: Easing.OutCubic
        }
    }

    RowLayout {
        anchors.fill: parent
        anchors.leftMargin: root.contentVisible
                            ? root.depth * Theme.treeDepthStep : 0
        spacing: Theme.spaceSm
        z: Theme.zRaised

        TreeChevron {
            Layout.preferredWidth: Theme.iconSm
            Layout.preferredHeight: Theme.iconSm
            visible: root.contentVisible && root.directory && !root.loading
            expanded: root.expanded
            interactive: true
            onToggled: root.toggled()
        }

        LoadingSpinner {
            Layout.preferredWidth: Theme.iconSm
            Layout.preferredHeight: Theme.iconSm
            visible: root.contentVisible && root.directory && root.loading
            active: visible
            size: Theme.iconSm
        }

        Item {
            Layout.preferredWidth: Theme.iconSm
            Layout.preferredHeight: Theme.iconSm
            visible: root.contentVisible && !root.directory
        }

        FileIcon {
            visible: root.contentVisible
            fileName: root.fileName
            isDir: root.directory
            expanded: root.expanded
            isRoot: root.rootDirectory
            size: Theme.iconMd
            color: root.gitColor(root.effectiveGitStatus)
        }

        UiText {
            visible: root.contentVisible
            Layout.fillWidth: true
            Layout.minimumWidth: 0
            size: "md"
            text: root.fileName
            color: root.gitColor(root.effectiveGitStatus)
            elide: Text.ElideRight
        }

        UiText {
            visible: root.contentVisible && root.statusVisible
                     && (root.statusText.length > 0
                         || (root.effectiveGitStatus.length > 0
                             && root.effectiveGitStatus !== "ignored"))
            text: root.statusText.length > 0
                  ? root.statusText : root.gitLabel(root.effectiveGitStatus)
            color: root.gitColor(root.effectiveGitStatus)
            size: "sm"
        }
    }
}
