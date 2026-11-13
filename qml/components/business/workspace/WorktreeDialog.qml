import QtQuick
import QtQuick.Dialogs
import QtQuick.Layouts
import SpanTui

// Creates a worktree of a main project on a new branch. The location follows
// `<worktree root>\<project>\<name>` until it is edited by hand; the base
// branch defaults to the project's current branch.
AppDialog {
    id: dialog

    property string projectPath: ""
    property var branches: []
    property bool loadingBranches: false
    // The location was typed or picked; the name no longer drives it.
    property bool locationEdited: false

    readonly property string name: nameField.text.trim()
    readonly property string base: baseBox.currentIndex >= 0 && baseBox.currentIndex < branches.length
                                   ? branches[baseBox.currentIndex] : ""
    readonly property string problem: {
        if (name.length === 0)
            return ""
        const nameProblem = WorkspaceController.worktreeNameProblem(name)
        if (nameProblem.length > 0)
            return nameProblem
        if (branches.indexOf(name) >= 0)
            return qsTr("A branch named “%1” already exists.").arg(name)
        return WorkspaceController.worktreeLocationProblem(locationField.text)
    }

    function openFor(path) {
        projectPath = path
        branches = []
        loadingBranches = true
        locationEdited = false
        nameField.text = ""
        locationField.text = WorkspaceController.worktreeLocation(WorkspaceSettings.worktreeRoot, path, "")
        baseBox.currentIndex = -1
        WorkspaceController.requestWorktreeBases(path)
        open()
    }

    function accept() {
        if (!confirmEnabled)
            return
        if (WorkspaceController.createWorktree(projectPath, name, base, locationField.text.trim()))
            close()
    }

    heading: qsTr("New worktree")
    size: "md"
    accentRim: true
    confirmText: qsTr("Create")
    confirmEnabled: name.length > 0 && base.length > 0 && problem.length === 0
                    && !WorkspaceController.worktreeBusy

    onCancelled: dialog.close()
    onConfirmed: dialog.accept()
    onOpened: nameField.forceActiveFocus()

    Connections {
        target: WorkspaceController
        function onWorktreeBasesReady(projectPath, branches, current) {
            if (projectPath !== dialog.projectPath)
                return
            dialog.branches = branches
            dialog.loadingBranches = false
            baseBox.currentIndex = Math.max(0, branches.indexOf(current))
        }
    }

    GridLayout {
        Layout.fillWidth: true
        columns: 2
        columnSpacing: Theme.spaceMd
        rowSpacing: Theme.spaceMd

        UiText { text: qsTr("Name"); tone: "muted" }
        UiField {
            id: nameField
            Layout.fillWidth: true
            role: "mono"
            placeholderText: qsTr("Also the new branch, e.g. feature/login")
            onTextChanged: {
                if (!dialog.locationEdited)
                    locationField.text = WorkspaceController.worktreeLocation(
                        WorkspaceSettings.worktreeRoot, dialog.projectPath, text)
            }
            onAccepted: dialog.accept()
        }

        UiText { text: qsTr("From branch"); tone: "muted" }
        AppComboBox {
            id: baseBox
            Layout.fillWidth: true
            model: dialog.branches
            leadingIcon: "git-branch"
            placeholderText: dialog.loadingBranches ? qsTr("Loading branches...")
                                                    : qsTr("No branches found")
            onActivated: index => baseBox.currentIndex = index
        }

        UiText { text: qsTr("Location"); tone: "muted" }
        UiField {
            id: locationField
            Layout.fillWidth: true
            role: "mono"
            fadeOverflow: true
            onTextEdited: dialog.locationEdited = true
            onAccepted: dialog.accept()
            trailing: ChromeButton {
                kind: "ghost"
                size: "sm"
                iconName: "folder"
                tip: qsTr("Choose the parent folder")
                onClicked: locationPicker.open()
            }
        }

        Item {}
        UiText {
            Layout.fillWidth: true
            text: qsTr("The default folder for new worktrees is set in Settings > Environment.")
            tone: "muted"
            size: "sm"
            wrapMode: Text.WordWrap
        }
    }

    UiText {
        Layout.fillWidth: true
        visible: dialog.problem.length > 0
        text: dialog.problem
        tone: "danger"
        size: "sm"
        wrapMode: Text.WordWrap
    }

    // Picks the folder the worktree goes into; its own folder is named
    // after the worktree inside it.
    FolderDialog {
        id: locationPicker
        title: qsTr("Choose where to create the worktree")
        onAccepted: {
            const container = WorkspaceController.folderPathFromUrl(selectedFolder)
            if (container.length === 0)
                return
            const folder = WorkspaceController.worktreeFolderName(dialog.name)
            locationField.text = folder.length > 0 ? container + "\\" + folder : container
            dialog.locationEdited = true
        }
    }
}
