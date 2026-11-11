pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Layouts
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

    // Hairlines sit above the dock surface, matching the tile seam tier.
    readonly property int layerSeam: 30

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

    // Body on the left, page rail on the right edge: the rail takes a
    // narrow column instead of a header row, so every page keeps the full
    // dock height and the switch never has to fold its labels.
    RowLayout {
        anchors.fill: parent
        spacing: 0

        StackLayout {
            id: dockBody
            Layout.fillWidth: true
            Layout.fillHeight: true
            // The dock owns the body gutter on all sides (matching the
            // sidebar's spaceMd list inset). Pages fill the inset area and
            // keep only their own internal padding.
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

        PixelLine {
            visible: !root.remote && root.gitPages
            vertical: true
            Layout.fillHeight: true
            Layout.preferredWidth: implicitWidth
            lineColor: Theme.separator
        }

        // Page rail: one icon button per page, the current one toggled on;
        // labels live in the tips.
        Item {
            visible: !root.remote && root.gitPages
            Layout.fillHeight: true
            Layout.preferredWidth: Theme.sizeMd + Theme.spaceSm * 2

            Column {
                anchors.top: parent.top
                anchors.topMargin: Theme.spaceMd
                anchors.horizontalCenter: parent.horizontalCenter
                spacing: Theme.spaceSm

                Repeater {
                    model: [
                        { key: "files", text: qsTr("Files"), iconName: "folder" },
                        { key: "diff", text: qsTr("Changes"), iconName: "git-commit" },
                        { key: "history", text: qsTr("History"), iconName: "history" }
                    ]
                    delegate: ChromeButton {
                        required property var modelData
                        iconName: modelData.iconName
                        tip: modelData.text
                        kind: root.page === modelData.key ? "selected" : "ghost"
                        onClicked: WorkspaceLayout.setRightPage(modelData.key)
                    }
                }
            }
        }
    }
}
