pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Dialogs
import QtQuick.Layouts
import SpanTui

// Remote file browser in the right dock while a remote terminal is focused:
// the SFTP-client layout (address bar, toolbar, detail list sorted with
// folders first) instead of the local project tree. Every listing and
// mutation goes through RemoteFileModel; this pane keeps only the row
// selection.
Item {
    id: root

    required property RemoteFileModel fileModel
    required property string hostName
    property string selectedPath: ""
    readonly property bool hasModel: fileModel !== null
    readonly property bool connecting: hasModel && fileModel.connectionState === "connecting"
    readonly property bool failed: hasModel && fileModel.connectionState === "failed"
    // Detail columns appear once the dock is wide enough to keep the names
    // readable.
    readonly property bool wide: width >= Theme.dialogSm
    readonly property int sizeColumn: Theme.sizeLg * 2
    readonly property int dateColumn: Theme.sizeLg * 3

    onFileModelChanged: {
        selectedPath = ""
        if (fileModel)
            fileModel.activate()
    }
    Component.onCompleted: if (fileModel) fileModel.activate()

    Connections {
        target: root.fileModel
        function onCurrentPathChanged(): void { root.selectedPath = "" }
    }

    function newFolder(): void {
        nameDialog.openFor(qsTr("New folder"), qsTr("folder"), qsTr("Create"), function(name) {
            fileModel.createFolder(name)
        })
    }

    function newFile(): void {
        nameDialog.openFor(qsTr("New file"), qsTr("name.txt"), qsTr("Create"), function(name) {
            fileModel.createFile(name)
        })
    }

    function promptRename(path: string, name: string): void {
        nameDialog.openFor(qsTr("Rename"), qsTr("new name"), qsTr("Rename"), function(newName) {
            fileModel.rename(path, newName)
        }, name)
    }

    function confirmDelete(path: string, name: string, isDir: bool): void {
        deleteConfirm.openFor(
            isDir ? qsTr("Delete folder") : qsTr("Delete file"),
            isDir ? qsTr("Permanently delete “%1” and everything in it on the server?").arg(name)
                  : qsTr("Permanently delete “%1” on the server?").arg(name),
            qsTr("Delete"),
            function() { fileModel.remove([path]) })
    }

    function download(paths: var): void {
        downloadDialog.pendingPaths = paths
        downloadDialog.open()
    }

    function sortMark(key: string): string {
        if (!hasModel || fileModel.sortKey !== key)
            return ""
        return fileModel.sortAscending ? " ↑" : " ↓"
    }

    function openBackgroundMenu(source: Item, x: real, y: real): void {
        contextMenu.openAt(source, x, y, [
            { text: qsTr("New folder"), icon: "folder-plus", action: root.newFolder },
            { text: qsTr("New file"), icon: "file-plus", action: root.newFile },
            { text: qsTr("Upload files…"), icon: "upload", action: function() { uploadDialog.open() } },
            { separator: true },
            { text: qsTr("Refresh"), icon: "refresh", action: function() { fileModel.refresh() } },
            { text: qsTr("Copy path"), icon: "copy",
              action: function() { fileModel.copyPathToClipboard(fileModel.currentPath) } }
        ])
    }

    function openRowMenu(source: Item, x: real, y: real, row: int, path: string, name: string,
                         isDir: bool): void {
        let items = []
        if (isDir)
            items.push({ text: qsTr("Open"), icon: "folder-open", action: function() { fileModel.openRow(row) } })
        items.push(
            { text: qsTr("Download…"), icon: "download", action: function() { root.download([path]) } },
            { text: qsTr("Rename"), icon: "pencil", action: function() { root.promptRename(path, name) } },
            { text: qsTr("Copy path"), icon: "copy", action: function() { fileModel.copyPathToClipboard(path) } },
            { separator: true },
            { text: qsTr("Delete"), icon: "trash", danger: true,
              action: function() { root.confirmDelete(path, name, isDir) } }
        )
        contextMenu.openAt(source, x, y, items)
    }

    ColumnLayout {
        anchors.fill: parent
        spacing: Theme.spaceSm

        // Toolbar: navigation on the left, file actions on the right.
        RowLayout {
            Layout.fillWidth: true
            Layout.preferredHeight: Theme.sizeSm
            spacing: Theme.spaceSm

            ChromeButton {
                size: "sm"; kind: "ghost"; iconName: "arrow-up"; tip: qsTr("Parent folder")
                enabled: root.hasModel && root.fileModel.currentPath.length > 1
                onClicked: root.fileModel.goUp()
            }
            ChromeButton {
                size: "sm"; kind: "ghost"; iconName: "home"; tip: qsTr("Home folder")
                enabled: root.hasModel
                onClicked: root.fileModel.goHome()
            }
            ChromeButton {
                size: "sm"; kind: "ghost"; iconName: "refresh"
                tip: root.failed ? qsTr("Reconnect") : qsTr("Refresh")
                enabled: root.hasModel
                onClicked: root.failed ? root.fileModel.reconnect() : root.fileModel.refresh()
            }
            Item { Layout.fillWidth: true }
            ChromeButton {
                size: "sm"; kind: "ghost"; iconName: "folder-plus"; tip: qsTr("New folder")
                enabled: root.hasModel && root.fileModel.currentPath.length > 0
                onClicked: root.newFolder()
            }
            ChromeButton {
                size: "sm"; kind: "ghost"; iconName: "upload"; tip: qsTr("Upload files…")
                enabled: root.hasModel && root.fileModel.currentPath.length > 0
                onClicked: uploadDialog.open()
            }
            ChromeButton {
                size: "sm"; kind: "ghost"; iconName: "download"; tip: qsTr("Download selected…")
                enabled: root.selectedPath.length > 0
                onClicked: root.download([root.selectedPath])
            }
            ChromeButton {
                size: "sm"
                kind: root.hasModel && root.fileModel.showHidden ? "selected" : "ghost"
                iconName: "files"
                tip: qsTr("Show hidden files")
                enabled: root.hasModel
                onClicked: root.fileModel.showHidden = !root.fileModel.showHidden
            }
        }

        // Address bar: the directory being shown; Enter opens a typed path
        // ("~" is the remote home).
        UiField {
            id: pathField
            Layout.fillWidth: true
            size: "sm"
            textSize: "sm"
            role: "mono"
            leadingIcon: "server"
            fadeOverflow: true
            enabled: root.hasModel
            placeholderText: root.connecting ? qsTr("Connecting to %1…").arg(root.hostName)
                                             : qsTr("Remote path")
            text: root.hasModel ? root.fileModel.currentPath : ""
            // Typing keeps the binding; an assignment would cut the field off
            // from later navigation, so leaving the edit re-binds it instead.
            function followCurrentPath(): void {
                text = Qt.binding(function() { return root.hasModel ? root.fileModel.currentPath : "" })
                focus = false
            }
            onAccepted: {
                const typed = text
                followCurrentPath()
                root.fileModel.openPath(typed)
            }
            Keys.onEscapePressed: followCurrentPath()
        }

        InlineBanner {
            Layout.fillWidth: true
            inset: true
            dismissible: true
            textSize: "md"
            showIcon: false
            active: root.hasModel && root.fileModel.errorMessage.length > 0
            text: root.hasModel ? root.fileModel.errorMessage : ""
            onDismissed: root.fileModel.clearError()
        }

        // Column header; clicking a column sorts by it, again reverses.
        RowLayout {
            Layout.fillWidth: true
            // Floor only: a larger aux font grows the header row.
            Layout.minimumHeight: Theme.sizeSm
            Layout.leftMargin: Theme.spaceMd
            Layout.rightMargin: Theme.spaceMd
            spacing: Theme.spaceSm
            visible: root.hasModel && root.fileModel.count > 0

            UiText {
                Layout.fillWidth: true
                size: "sm"
                tone: "muted"
                text: qsTr("Name") + root.sortMark("name")
                elide: Text.ElideRight
                TapHandler { onTapped: root.fileModel.sortBy("name") }
            }
            UiText {
                Layout.preferredWidth: root.sizeColumn
                visible: root.wide
                size: "sm"
                tone: "muted"
                horizontalAlignment: Text.AlignRight
                text: qsTr("Size") + root.sortMark("size")
                TapHandler { onTapped: root.fileModel.sortBy("size") }
            }
            UiText {
                Layout.preferredWidth: root.dateColumn
                visible: root.wide
                size: "sm"
                tone: "muted"
                text: qsTr("Modified") + root.sortMark("modified")
                TapHandler { onTapped: root.fileModel.sortBy("modified") }
            }
        }

        Item {
            Layout.fillWidth: true
            Layout.fillHeight: true

            readonly property bool firstLoad: root.hasModel && root.fileModel.loading
                                              && fileList.count === 0

            EmptyState {
                anchors.fill: parent
                active: !parent.firstLoad && fileList.count === 0
                icon: root.failed ? "alert" : "folder"
                title: root.failed ? qsTr("Not connected") : qsTr("This folder is empty")
                subtitle: root.failed ? qsTr("Check the host settings, then reconnect")
                                      : qsTr("Drop files here to upload them")
                actionText: root.failed ? qsTr("Reconnect") : ""
                actionIcon: root.failed ? "refresh" : ""
                onActionTriggered: root.fileModel.reconnect()
            }

            // The list bleeds through the dock's right gutter so its
            // scrollbar sits on the dock edge; rows keep the gutter.
            DesktopListView {
                id: fileList
                anchors.fill: parent
                anchors.rightMargin: -Theme.spaceMd
                model: root.fileModel
                visible: count > 0
                glideHover: true

                MouseArea {
                    anchors.fill: parent
                    z: Theme.zBelow
                    acceptedButtons: Qt.RightButton
                    onClicked: mouse => root.openBackgroundMenu(this, mouse.x, mouse.y)
                }

                delegate: ListItem {
                    id: row

                    required property int index
                    required property string name
                    required property string path
                    required property bool isDir
                    required property bool isLink
                    required property string sizeText
                    required property string modifiedText
                    required property string permissions
                    required property string owner

                    width: fileList.width - Theme.spaceMd
                    selected: root.selectedPath === path
                    cursorShape: isDir ? Qt.PointingHandCursor : Qt.ArrowCursor

                    onPressed: mouse => {
                        if (mouse.button === Qt.LeftButton)
                            root.selectedPath = path
                    }
                    onClicked: mouse => {
                        if (mouse.button === Qt.RightButton)
                            root.openRowMenu(row.mouseArea, mouse.x, mouse.y,
                                             index, path, name, isDir)
                    }
                    onDoubleClicked: mouse => {
                        if (mouse.button !== Qt.LeftButton)
                            return
                        if (isDir)
                            root.fileModel.openRow(index)
                        else
                            root.download([path])
                    }

                    Tip {
                        visible: row.hovered && !root.wide
                        text: [row.sizeText, row.modifiedText, row.permissions, row.owner]
                              .filter(function(part) { return part.length > 0 }).join("  ·  ")
                    }

                    RowLayout {
                        anchors.fill: parent
                        spacing: Theme.spaceSm

                        FileIcon {
                            Layout.preferredWidth: Theme.iconMd
                            Layout.preferredHeight: Theme.iconMd
                            fileName: row.name
                            isDir: row.isDir
                            size: Theme.iconMd
                        }
                        UiText {
                            Layout.fillWidth: true
                            text: row.name
                            tone: row.isLink ? "accent" : "default"
                            elide: Text.ElideMiddle
                        }
                        UiText {
                            Layout.preferredWidth: root.sizeColumn
                            visible: root.wide
                            size: "sm"
                            tone: "muted"
                            horizontalAlignment: Text.AlignRight
                            text: row.sizeText
                        }
                        UiText {
                            Layout.preferredWidth: root.dateColumn
                            visible: root.wide
                            size: "sm"
                            tone: "muted"
                            text: row.modifiedText
                            elide: Text.ElideRight
                        }
                    }
                }
            }

            // Local files dropped from Explorer upload into this directory.
            DropArea {
                anchors.fill: parent
                enabled: root.hasModel && root.fileModel.currentPath.length > 0
                onDropped: drop => {
                    if (drop.hasUrls) {
                        root.fileModel.upload(drop.urls)
                        drop.accept()
                    }
                }
            }

            PaneLoading {
                anchors.fill: parent
                active: parent.firstLoad
                message: root.connecting ? qsTr("Connecting to %1…").arg(root.hostName)
                                         : qsTr("Loading files…")
            }
        }
    }

    FileDialog {
        id: uploadDialog
        title: qsTr("Upload files")
        fileMode: FileDialog.OpenFiles
        onAccepted: root.fileModel.upload(selectedFiles)
    }

    FolderDialog {
        id: downloadDialog
        property var pendingPaths: []
        title: qsTr("Download to folder")
        onAccepted: root.fileModel.download(pendingPaths, selectedFolder)
    }

    NameDialog { id: nameDialog }
    AppContextMenu { id: contextMenu }
    ConfirmDialog { id: deleteConfirm }
}
