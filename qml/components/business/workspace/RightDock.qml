pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Layouts
import QtQuick.Window
import SpanTui

Item {
    id: root

    readonly property string page: WorkspaceLayout.rightPage
    readonly property string projectPath: WorkspaceController.boundTerminalProjectPath
    readonly property ProjectContext projectContext: projectPath.length > 0
        ? WorkspaceController.projectContext(projectPath) : null
    readonly property FileTreeModel projectFileTree: projectContext ? projectContext.fileTree : null
    readonly property GitRepository projectGit: projectContext ? projectContext.gitRepository : null
    // A remote session binds a saved SSH host instead: the dock then shows
    // only that host's file browser — remote hosts have no Git pages.
    readonly property string remoteHostId: WorkspaceController.boundRemoteHostId
    readonly property bool remote: remoteHostId.length > 0
    readonly property RemoteHostContext remoteContext: remote ? WorkspaceController.remoteContext(remoteHostId)
                                                              : null
    // Without a usable Git the Changes and History pages do not exist: the
    // dock shows the file tree alone and drops its page rail.
    readonly property bool gitPages: Tools.gitAvailable
    readonly property real renderDpr: PixelGrid.safeDpr(Screen.devicePixelRatio)
    readonly property Window hostWindow: Window.window
    readonly property real topBandHeight: hostWindow && hostWindow.topBarHeight > 0
                                          ? hostWindow.topBarHeight : Theme.paneHeaderH

    // Hairlines sit above the dock surface, matching the tile seam tier.
    readonly property int layerSeam: 30

    Component.onCompleted: {
        if (WindowChrome && WindowChrome.addInteractiveItem)
            WindowChrome.addInteractiveItem(pageTabs)
    }

    Rectangle {
        anchors.fill: parent
        color: Theme.surfaceRaised
    }

    // The dock owns the zone seam on its left edge and the window-edge
    // rails on the right/bottom — same single-hairline rule as tiles.
    PixelLine {
        z: root.layerSeam
        vertical: true
        anchors.left: parent.left
        anchors.top: parent.top
        anchors.bottom: parent.bottom
        lineColor: Theme.border
    }
    PixelLine {
        z: root.layerSeam
        vertical: true
        anchors.right: parent.right
        anchors.top: parent.top
        anchors.bottom: parent.bottom
        lineColor: Theme.border
    }
    PixelLine {
        z: root.layerSeam
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        lineColor: Theme.border
    }

    // Header shares the window's unified top-band height so its bottom hairline
    // continues the sidebar and center tiles' header line cleanly across the
    // window, followed by the full-width page body.
    ColumnLayout {
        anchors.fill: parent
        spacing: 0

        Item {
            id: dockHeader
            Layout.fillWidth: true
            Layout.preferredHeight: root.topBandHeight

            SegmentedControl {
                id: pageTabs
                visible: !root.remote && root.gitPages
                anchors.left: parent.left
                anchors.leftMargin: Theme.spaceMd + Theme.spaceSm
                anchors.verticalCenter: parent.verticalCenter
                size: "md"
                tone: "subtle"
                iconCellWidth: Theme.sizeMd + Theme.spaceSm
                current: root.page
                model: [
                    { key: "files", iconName: "folder", tip: qsTr("Files") },
                    { key: "diff", iconName: "git-branch", tip: qsTr("Changes") },
                    { key: "history", iconName: "history", tip: qsTr("History") }
                ]
                onPicked: key => WorkspaceLayout.setRightPage(String(key))
            }

            UiText {
                visible: root.remote || !root.gitPages
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.leftMargin: Theme.spaceMd + Theme.spaceSm
                anchors.rightMargin: Theme.spaceMd + (root.hostWindow ? root.hostWindow.topRightInset : 0)
                anchors.verticalCenter: parent.verticalCenter
                size: "md"
                text: qsTr("Files")
                elide: Text.ElideRight
            }
        }

        PixelLine {
            Layout.fillWidth: true
            Layout.preferredHeight: implicitHeight
            lineColor: Theme.border
        }

        StackLayout {
            id: dockBody
            Layout.fillWidth: true
            Layout.fillHeight: true
            // The dock body owns the page gutter on all sides (matching the
            // sidebar's spaceMd list inset).
            Layout.margins: Theme.spaceMd
            // Both bodies stay alive: a Loader swap destroys the old page and
            // can abort in-flight delegate incubation inside its lists, which
            // surfaces "Cannot create delegate / destroyed during incubation"
            // warnings as message toasts.
            currentIndex: root.remote ? 2 : (root.page === "files" || !root.gitPages ? 1 : 0)

            GitPane {
                repository: root.projectGit
                section: root.page
            }

            FileTreePane {
                fileTreeModel: root.projectFileTree
                projectPath: root.projectPath
            }

            RemoteFilePane {
                fileModel: root.remoteContext ? root.remoteContext.fileModel : null
                hostName: {
                    const _ = RemoteHosts.count
                    return root.remote ? RemoteHosts.hostName(root.remoteHostId) : ""
                }
            }
        }
    }
}
