pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Layouts
import SpanTui

Item {
    id: root

    required property FileTreeModel fileTreeModel
    required property string projectPath
    // Toolbar actions target the selected path; empty means the project root.
    property string selectedPath: ""
    readonly property bool hasProject: projectPath.length > 0

    onProjectPathChanged: selectedPath = ""

    function categoryColor(category: string): color {
        if (category === "build") return Theme.categoryBuild
        if (category === "test") return Theme.categoryTest
        if (category === "dependency") return Theme.categoryDependency
        if (category === "docs") return Theme.categoryDocs
        if (category === "assets") return Theme.categoryAssets
        if (category === "config") return Theme.categoryConfig
        return "transparent"
    }

    function newFile(parentPath: string): void {
        nameDialog.openFor(qsTr("New file"), qsTr("name.txt"), qsTr("Create"), function(name) {
            fileTreeModel.createFile(parentPath, name)
        })
    }

    function newFolder(parentPath: string): void {
        nameDialog.openFor(qsTr("New folder"), qsTr("folder"), qsTr("Create"), function(name) {
            fileTreeModel.createFolder(parentPath, name)
        })
    }

    function promptRename(rowPath: string, currentName: string): void {
        nameDialog.openFor(qsTr("Rename"), qsTr("new name"), qsTr("Rename"), function(name) {
            fileTreeModel.renamePath(rowPath, name)
        }, currentName)
    }

    function runScript(row: int, rowName: string): void {
        const commandLine = fileTreeModel.projectTerminalCommand(row)
        const projectPath = root.projectPath
        if (commandLine.length === 0 || projectPath.length === 0)
            return
        if (WorkspaceSettings.scriptRunMode === "independentWindow") {
            fileTreeModel.runInIndependentTerminal(row)
            return
        }
        const sessionId = SessionModel.addCommandSession(
            qsTr("Run “%1”").arg(rowName), projectPath, projectPath,
            commandLine)
        WorkspaceController.showSession(sessionId)
    }

    // Right-click never moves the selection: the menu marks its target row
    // (AppContextMenu → ListItem.menuTarget) and every action takes the
    // clicked path explicitly.
    function openRootMenu(source: Item, x: real, y: real): void {
        contextMenu.openAt(source, x, y, [
            { text: qsTr("New file"), icon: "file-plus", action: function() { root.newFile(root.projectPath) } },
            { text: qsTr("New folder"), icon: "folder-plus", action: function() { root.newFolder(root.projectPath) } },
            { separator: true },
            { text: qsTr("Refresh tree"), icon: "refresh", action: function() { fileTreeModel.refresh() } },
            {
                text: qsTr("Undo last delete"), icon: "undo", enabled: fileTreeModel.canUndo,
                action: function() { fileTreeModel.undoLastDelete() }
            },
            { separator: true },
            {
                text: qsTr("Open in File Explorer"), icon: "external-link",
                action: function() { fileTreeModel.openPathExternally(root.projectPath) }
            }
        ])
    }

    function openRowMenu(source: Item, x: real, y: real, row: int, rowName: string, rowPath: string,
                         directory: bool, rootNode: bool): void {
        if (rootNode) {
            openRootMenu(source, x, y)
            return
        }
        if (directory) {
            contextMenu.openAt(source, x, y, [
                { text: qsTr("New file"), icon: "file-plus", action: function() { root.newFile(rowPath) } },
                { text: qsTr("New folder"), icon: "folder-plus", action: function() { root.newFolder(rowPath) } },
                { separator: true },
                {
                    text: qsTr("Open in File Explorer"), icon: "external-link",
                    action: function() { fileTreeModel.openPathExternally(rowPath) }
                },
                { text: qsTr("Rename"), icon: "pencil", action: function() { root.promptRename(rowPath, rowName) } },
                { separator: true },
                {
                    text: qsTr("Delete"), icon: "trash", danger: true,
                    action: function() { root.confirmDelete(rowPath, rowName, true) }
                }
            ])
            return
        }
        let items = [
            { text: qsTr("Open"), icon: "file",
              action: function() {
                  WorkspaceController.openFile(root.projectPath, rowPath)
              } },
        ]
        if (fileTreeModel.canRunInProjectTerminal(row)) {
            const independent = WorkspaceSettings.scriptRunMode === "independentWindow"
            items.push({
                text: independent
                      ? qsTr("Run “%1” in independent window").arg(rowName)
                      : qsTr("Run “%1” in project terminal").arg(rowName),
                icon: "play",
                action: function() { root.runScript(row, rowName) }
            })
            items.push({ separator: true })
        }
        items.push(
            { text: qsTr("Open in File Explorer"), icon: "external-link", action: function() { fileTreeModel.openPathExternally(rowPath) } },
            { text: qsTr("Rename"), icon: "pencil", action: function() { root.promptRename(rowPath, rowName) } },
            { separator: true },
            {
                text: qsTr("Delete"), icon: "trash", danger: true,
                action: function() { root.confirmDelete(rowPath, rowName, false) }
            }
        )
        contextMenu.openAt(source, x, y, items)
    }

    function confirmDelete(rowPath: string, rowName: string, isDirectory: bool): void {
        if (isDirectory) {
            deleteConfirm.openFor(
                qsTr("Delete folder"),
                qsTr("Move “%1” and its contents to trash?").arg(rowName),
                qsTr("Delete"),
                function() { fileTreeModel.deletePath(rowPath) })
            return
        }
        fileTreeModel.deletePath(rowPath)
    }

    ColumnLayout {
        anchors.fill: parent
        // No margins: the dock body owns the pane gutter; selection/hover
        // strips run to the inset edge.
        spacing: Theme.spaceSm

        // Pane toolbar — the dock's segmented switch owns the header strip,
        // so file actions live here, flush with the tree rows' highlight edge
        // (ListItem's spaceSm surface inset) and the switch above.
        Item {
            Layout.fillWidth: true
            Layout.preferredHeight: Theme.sizeSm
            visible: root.hasProject

            Row {
                anchors.right: parent.right
                anchors.rightMargin: Theme.spaceSm
                anchors.verticalCenter: parent.verticalCenter
                spacing: Theme.spaceSm
                ChromeButton {
                    size: "sm"; kind: "ghost"; iconName: "file-plus"; tip: qsTr("New file")
                    enabled: root.fileTreeModel !== null
                    onClicked: root.newFile(root.selectedPath)
                }
                ChromeButton {
                    size: "sm"; kind: "ghost"; iconName: "folder-plus"; tip: qsTr("New folder")
                    enabled: root.fileTreeModel !== null
                    onClicked: root.newFolder(root.selectedPath)
                }
                ChromeButton {
                    size: "sm"; kind: "ghost"; iconName: "refresh"; tip: qsTr("Refresh tree")
                    enabled: root.fileTreeModel !== null
                    onClicked: root.fileTreeModel.refresh()
                }
                ChromeButton {
                    size: "sm"; kind: "ghost"; iconName: "undo"; tip: qsTr("Undo last delete")
                    enabled: Boolean(root.fileTreeModel && root.fileTreeModel.canUndo)
                    onClicked: root.fileTreeModel.undoLastDelete()
                }
            }
        }

        // Floating card: sits one gutter step deeper than the list strips.
        InlineBanner {
            Layout.fillWidth: true
            inset: true
            dismissible: true
            textSize: "md"
            showIcon: false
            active: Boolean(root.fileTreeModel && root.fileTreeModel.errorMessage.length > 0)
            text: root.fileTreeModel ? root.fileTreeModel.errorMessage : ""
            onDismissed: if (root.fileTreeModel) root.fileTreeModel.clearError()
        }
        Item {
            Layout.fillWidth: true
            Layout.fillHeight: true

            readonly property bool treePageLoading: Boolean(root.fileTreeModel && root.fileTreeModel.busy
                                                            && fileList.count <= 1)
            EmptyState {
                anchors.fill: parent
                active: !parent.treePageLoading
                        && (!root.hasProject || fileList.count === 0)
                icon: root.hasProject ? "files" : "folder"
                title: root.hasProject ? qsTr("This folder is empty") : qsTr("No project selected")
                subtitle: root.hasProject
                          ? qsTr("Use the toolbar or right-click to add files")
                          : qsTr("Select a project in the sidebar")
            }

            // The list bleeds through the dock's right gutter so its
            // scrollbar sits on the dock edge; rows keep the gutter.
            TreeListView {
                id: fileList
                anchors.fill: parent
                anchors.rightMargin: -Theme.spaceMd
                model: root.fileTreeModel
                visible: root.hasProject && count > 0 && !parent.treePageLoading
                glideHover: true

                MouseArea {
                    anchors.fill: parent
                    z: Theme.zBelow
                    acceptedButtons: Qt.RightButton
                    onClicked: mouse => root.openRootMenu(this, mouse.x, mouse.y)
                }

                delegate: GitFileTreeRow {
                    id: row

                    required property int index
                    required property var model

                    width: fileList.width - Theme.spaceMd
                    selected: root.selectedPath === model.path
                    categoryColor: root.categoryColor(model.category)
                    cursorShape: model.isDir ? Qt.PointingHandCursor : Qt.ArrowCursor
                    fileName: model.name
                    directory: model.isDir
                    depth: model.depth
                    expanded: model.expanded
                    revealed: model.revealed
                    animateReveal: true
                    recycleKey: row.index
                    loading: model.loading
                    rootDirectory: model.isRoot
                    gitStatus: model.gitStatus
                    aggregateGitStatus: model.aggregateGitStatus

                    onPressed: mouse => {
                        if (mouse.button === Qt.LeftButton) {
                            root.selectedPath = model.path
                        }
                    }
                    onToggled: root.fileTreeModel.toggleExpanded(row.index)
                    onClicked: mouse => {
                        if (mouse.button === Qt.RightButton) {
                            root.openRowMenu(row.mouseArea, mouse.x, mouse.y,
                                             row.index, model.name, model.path,
                                             model.isDir, model.isRoot)
                        } else if (!model.isDir) {
                            WorkspaceController.openFile(root.projectPath, model.path)
                        }
                    }
                    onDoubleClicked: mouse => {
                        if (mouse.button === Qt.LeftButton) {
                            if (model.isDir) {
                                root.fileTreeModel.toggleExpanded(row.index)
                            } else {
                                WorkspaceController.openFile(root.projectPath, model.path)
                            }
                        }
                    }

                }
            }

            PaneLoading {
                anchors.fill: parent
                active: parent.treePageLoading
                message: qsTr("Loading files…")
            }
        }
    }

    NameDialog { id: nameDialog }
    AppContextMenu { id: contextMenu }
    ConfirmDialog { id: deleteConfirm }
}
