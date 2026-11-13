import QtQuick
import QtQuick.Dialogs
import QtQuick.Layouts
import SpanTui

// Asked once, before the first worktree: the folder new worktrees go into
// (one subfolder per project). Settings > Environment changes it later; the
// create dialog can still pick another place per worktree.
AppDialog {
    id: dialog

    property string projectPath: ""
    readonly property string problem: WorkspaceController.worktreeRootProblem(rootField.text)

    // The folder is saved; the create dialog takes over for `projectPath`.
    signal chosen(string projectPath)

    function openFor(path) {
        projectPath = path
        rootField.text = WorkspaceController.suggestedWorktreeRoot(path)
        open()
    }

    function accept() {
        if (!confirmEnabled)
            return
        WorkspaceSettings.worktreeRoot = rootField.text.trim()
        if (WorkspaceSettings.worktreeRoot.length === 0)
            return
        close()
        chosen(projectPath)
    }

    heading: qsTr("Folder for worktrees")
    message: qsTr("New worktrees are created in this folder, one subfolder per project. You can change it later in Settings > Environment, or pick another place for a single worktree.")
    size: "md"
    accentRim: true
    confirmText: qsTr("Continue")
    confirmEnabled: problem.length === 0

    onCancelled: dialog.close()
    onConfirmed: dialog.accept()
    onOpened: rootField.forceActiveFocus()

    UiField {
        id: rootField
        Layout.fillWidth: true
        role: "mono"
        fadeOverflow: true
        onAccepted: dialog.accept()
        trailing: ChromeButton {
            kind: "ghost"
            size: "sm"
            iconName: "folder"
            tip: qsTr("Choose the worktree folder")
            onClicked: rootPicker.open()
        }
    }

    UiText {
        Layout.fillWidth: true
        visible: dialog.problem.length > 0 && rootField.text.length > 0
        text: dialog.problem
        tone: "danger"
        size: "sm"
        wrapMode: Text.WordWrap
    }

    FolderDialog {
        id: rootPicker
        title: qsTr("Choose the worktree folder")
        onAccepted: {
            const folder = WorkspaceController.folderPathFromUrl(selectedFolder)
            if (folder.length > 0)
                rootField.text = folder
        }
    }
}
