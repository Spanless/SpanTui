pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import QtQuick.Dialogs
import SpanTui

ApplicationWindow {
    id: window

    width: 1320  // style-ok: window default geometry
    height: 820  // style-ok: window default geometry
    minimumWidth: 980  // style-ok: window default geometry
    minimumHeight: 640  // style-ok: window default geometry
    flags: Qt.Window | Qt.FramelessWindowHint
    // The title band is drawn under the system title bar (macOS traffic
    // lights), so the content is not inset by its safe area.
    topPadding: 0
    visible: false
    title: Qt.application.displayName || qsTr("SpanTui")
    // Solid workspace paper. Panels and panes sit flush on it and carry the
    // hairline grid themselves; there is no ambient or material layer below.
    color: Theme.surfaceBase
    font: Appearance.uiFont(Appearance.uiFontSize, Appearance.fontRevision)

    readonly property real renderDpr: PixelGrid.safeDpr(Screen.devicePixelRatio)
    readonly property real topBarHeight: titleBar.height
    readonly property real topLeftInset: sideToggles.leftDocked ? 0 : (sideToggles.x + sideToggles.width)
    readonly property real topRightInset: windowControls.visible ? (windowControls.width + Theme.spaceMd) : 0
    readonly property bool interactionAnimationsEnabled:
        window.visible && window.active && !Appearance.reduceMotion
        && !(WindowTransition && WindowTransition.active)
    readonly property bool panelResizeActive:
        WindowTransition ? WindowTransition.active : false

    property alias projectFolderDialog: projectFolderDialog

    // Top-level view switch: "workspace" shows the canvas; "settings" opens
    // the modal Settings sheet over the window overlay.
    property string page: "workspace"
    // Screenshot-harness hook staged by the workspace-shot driver after load.
    property string shotRightPage: ""
    onShotRightPageChanged: {
        if (shotRightPage.length > 0) {
            WorkspaceLayout.setSideCollapsed("right", false)
            WorkspaceLayout.setRightPage(shotRightPage)
        }
    }

    // Sidebar rows anchor the app popups: the rail sits at the bottom-left,
    // so both popups flip open above their anchor row.
    function openInbox(item) { messagePopup.openBelow(item) }
    function openTaskPopup(item) { taskPopup.openBelow(item) }
    // The first worktree asks for the default worktree folder first.
    function openNewWorktree(projectPath) {
        if (WorkspaceSettings.worktreeRoot.length === 0)
            worktreeRootDialog.openFor(projectPath)
        else
            worktreeDialog.openFor(projectPath)
    }

    function syncAppearanceTheme() {
        Theme.dark = Appearance.effectiveDark
        Theme.uiFontSize = Appearance.uiFontSize
        Theme.uiAuxFontSize = Appearance.uiAuxFontSize
        Theme.formHintFontSize = Appearance.formHintFontSize
        Theme.editorFontSize = Appearance.editorFontSize
        WindowChrome.setDarkFrame(Theme.dark)
    }

    function syncTerminalTheme() {
        Theme.applyScheme(TerminalSettings.surfaceColors)
    }

    function syncTheme() {
        syncAppearanceTheme()
        syncTerminalTheme()
    }

    function syncAnimationPolicy() {
        Theme.animateColors = window.visible && !Appearance.reduceMotion
                              && !(WindowTransition && WindowTransition.active)
    }

    function confirmUpdate() {
        if (AppUpdate.busy || !AppUpdate.updateAvailable)
            return
        const from = AppUpdate.currentVersion || qsTr("current")
        const to = AppUpdate.availableVersion || qsTr("newer build")
        // Only an installed copy runs the installer; the others get the
        // update by hand from the release page or the local folder.
        if (!AppUpdate.canInstall) {
            updateConfirmDialog.openFor(
                qsTr("Download the update?"),
                (AppUpdate.portable
                 ? qsTr("A new version is available.\n\n%1  →  %2\n\nPortable copies do not install updates: unpack the new portable package and move the data folder of this copy into it.")
                 : qsTr("A new version is available.\n\n%1  →  %2\n\nA development build does not install updates."))
                    .arg(from).arg(to),
                qsTr("Open download page"),
                function() { Qt.openUrlExternally(AppUpdate.downloadPageUrl) })
        } else if (AppUpdate.localSourceEnabled) {
            updateConfirmDialog.openFor(
                qsTr("Install local update?"),
                qsTr("A newer local package is available.\n\n%1  →  %2\n\nInstaller:\n%3\n\nSpanTui will copy and verify the installer, then close; the installer shows its progress and restarts SpanTui.")
                    .arg(from).arg(to).arg(AppUpdate.sourceLocation),
                qsTr("Update & restart"),
                function() { AppUpdate.applyAndRestart() })
        } else {
            updateConfirmDialog.openFor(
                qsTr("Install stable release?"),
                qsTr("A new stable release is available on GitHub.\n\n%1  →  %2\n\nRelease:\n%3\n\nSpanTui will download and verify the installer, then close; the installer shows its progress and restarts SpanTui.")
                    .arg(from).arg(to).arg(AppUpdate.sourceLocation),
                qsTr("Update & restart"),
                function() { AppUpdate.applyAndRestart() })
        }
    }


    function requestUpdate() {
        // A running update reopens its progress card.
        if (AppUpdate.applying) {
            updateProgressDialog.open()
            return
        }
        if (!AppUpdate.canCheck) {
            MessageCenter.post("info", qsTr("Choose the local build folder in Settings > Developer first."))
            window.page = "settings"
            return
        }
        if (AppUpdate.updateAvailable) {
            window.confirmUpdate()
            return
        }
        AppUpdate.checkNow(false)
    }


    // The window's commands: the system menu bar runs them where there is one
    // (macOS), window shortcuts elsewhere — never both, so a chord runs once.
    AppCommands {
        id: appCommands
        window: window
    }
    Loader {
        active: Shortcuts.nativeMenuBar
        sourceComponent: MacMenuBar {
            commands: appCommands
        }
    }
    Loader {
        active: !Shortcuts.nativeMenuBar
        sourceComponent: AppShortcuts {
            commands: appCommands
        }
    }

    FolderDialog {
        id: projectFolderDialog
        title: qsTr("Choose a project folder")
        onAccepted: ProjectRegistry.addProjectUrl(selectedFolder)
    }

    Connections {
        target: Appearance
        function onReduceMotionChanged() { window.syncAnimationPolicy() }
        function onAppearanceChanged() { window.syncAppearanceTheme() }
    }

    Connections {
        target: TerminalSettings
        function onAppearanceChanged() { window.syncTerminalTheme() }
    }

    Connections {
        target: WindowTransition
        function onPhaseChanged() { window.syncAnimationPolicy() }
    }


    Component.onCompleted: {
        Theme.animateColors = false
        syncTheme()
        syncAnimationPolicy()
        WindowChrome.setTitleBarHeight(titleBar.height)
        WindowChrome.addInteractiveItem(sideToggles)
    }

    Item {
        id: titleBar
        z: Theme.zChrome
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: parent.top
        // Transparent chrome overlay on top of the workspace's top header band
        // (y = 0..topBarHeight). Holds window-anchored controls (macOS traffic
        // lights via the system, SidebarToggles, and Windows WindowControls)
        // while Sidebar, top-row center tiles and RightDock share one aligned
        // header row directly below.
        readonly property rect captionRect: WindowChrome.captionButtonsRect
        readonly property bool hasSystemCaption: captionRect.width > 0
        readonly property bool captionShown: hasSystemCaption && window.visibility !== Window.FullScreen
        height: PixelGrid.snapSize(
            Math.max(Theme.paneHeaderH,
                     hasSystemCaption ? 2 * (captionRect.y + captionRect.height / 2)
                                      : Theme.titlebarH),
            window.renderDpr)

        onHeightChanged: WindowChrome.setTitleBarHeight(height)

        SidebarToggles {
            id: sideToggles
            readonly property real captionEnd: titleBar.captionShown
                ? titleBar.captionRect.x + titleBar.captionRect.width : 0
            readonly property real minX: captionEnd > 0 ? captionEnd + Theme.spaceMd : Theme.spaceMd
            readonly property bool leftDocked: WorkspaceLayout.leftWidth > 0 && !WorkspaceLayout.leftFloating
            x: leftDocked
               ? Math.max(minX, WorkspaceLayout.leftWidth - width - Theme.spaceMd)
               : minX
            anchors.verticalCenter: parent.verticalCenter
            size: titleBar.height >= Theme.sizeMd ? "md" : "sm"
        }

        WindowControls {
            id: windowControls
            visible: !WindowChrome.systemCaptionButtons
            anchors.right: parent.right
            anchors.rightMargin: Theme.spaceMd
            anchors.verticalCenter: parent.verticalCenter
        }
    }

    WorkspaceCanvas {
        id: workspaceCanvas
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: parent.top
        anchors.bottom: parent.bottom
        z: Theme.zWorkspace
        interactionAnimationsEnabled: window.interactionAnimationsEnabled
    }

    // Settings is a modal sheet, not a tile: the Loader instantiates it on
    // demand and opens it over the window overlay; the canvas stays alive
    // and visible under the scrim, so pane state survives untouched.
    Loader {
        id: settingsPageLoader
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: titleBar.bottom
        anchors.bottom: parent.bottom
        active: window.page === "settings"
        sourceComponent: settingsPageComponent
        onLoaded: item.open()
    }
    Component {
        id: settingsPageComponent
        Settings {
            onPopupClosed: window.page = "workspace"
        }
    }

    MessageCenterPopup {
        id: messagePopup
        alignRight: false
        anchorFlip: true
    }
    TaskProgressPopup {
        id: taskPopup
        alignRight: false
        anchorFlip: true
    }
    ConfirmDialog {
        id: updateConfirmDialog
        danger: false
        z: Theme.zModal
    }
    UpdateProgressDialog {
        id: updateProgressDialog
        z: Theme.zModal
    }
    // Worktree dialogs live here, not in a sidebar tile: a sidebar may be
    // closed (or shown twice) while a Git command answers.
    WorktreeRootDialog {
        id: worktreeRootDialog
        z: Theme.zModal
        onChosen: projectPath => worktreeDialog.openFor(projectPath)
    }
    WorktreeDialog {
        id: worktreeDialog
        z: Theme.zModal
    }
    ConfirmDialog {
        id: worktreeForceDialog
        z: Theme.zModal
    }
    // ssh/sftp password, passphrase and host-key questions for remote hosts.
    SshPromptDialog {
        z: Theme.zModal
    }
    MessageToastStack {
        z: Theme.zToast
        anchors.right: parent.right
        anchors.top: titleBar.bottom
        anchors.rightMargin: Theme.spaceMd
        anchors.topMargin: Theme.spaceMd
        width: Math.min(implicitWidth, Math.max(0, parent.width - Theme.spaceMd * 2))
    }

    Connections {
        target: AppUpdate
        function onVisibleCheckFinished(available, message) {
            if (available)
                window.confirmUpdate()
            else if (message.length > 0)
                MessageCenter.post("info", message)
        }
        function onApplyStarted() { updateProgressDialog.open() }
    }

    // Git refused to delete a worktree (changes, untracked files): offer
    // the forced removal with the reason it gave.
    Connections {
        target: WorkspaceController
        function onWorktreeDeleteFailed(projectPath, error) {
            worktreeForceDialog.openFor(qsTr("Force delete worktree?"),
                qsTr("Git did not delete the worktree:\n\n%1\n\nDelete it anyway? Its uncommitted changes and untracked files are lost.").arg(error),
                qsTr("Force delete"),
                function() { WorkspaceController.deleteWorktree(projectPath, true) })
        }
    }

    Connections {
        target: TaskProgress
        function onCountChanged() {
            if (TaskProgress.count === 0 && taskPopup.opened)
                taskPopup.close()
        }
    }

}
