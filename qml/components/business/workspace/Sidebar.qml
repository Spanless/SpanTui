import QtQuick
import QtQuick.Layouts
import SpanTui

Item {
    id: root
    property int sessionRevision: 0
    readonly property Window hostWindow: Window.window
    readonly property real renderDpr: PixelGrid.safeDpr(Screen.devicePixelRatio)

    // Project and worktree rows share this menu. A project whose folder is
    // gone offers only its removal; a worktree row adds its own deletion.
    function openProjectMenu(source, x, y, projectPath, projectName) {
        // Folders deleted while the app was in the background show up now.
        ProjectRegistry.refreshFolders()
        OpenTargets.refresh()
        const mainProject = ProjectRegistry.parentProject(projectPath)
        const isWorktree = mainProject.length > 0
        if (!ProjectRegistry.isAvailable(projectPath)) {
            contextMenu.openAt(source, x, y, [
                { text: isWorktree ? qsTr("Remove from list") : qsTr("Remove project"),
                  icon: "trash", danger: true,
                  action: function() {
                      confirmDialog.openFor(qsTr("Remove project"),
                          qsTr("The folder of “%1” no longer exists. Remove it from the list and discard its sessions?").arg(projectName),
                          qsTr("Remove"),
                          function() { WorkspaceController.removeProject(projectPath) })
                  } }
            ])
            return
        }

        // "New terminal" starts the default shell on click; hovering opens
        // every startable shell, remembered by the session it creates.
        const profiles = Shells.launchProfiles
        const defaultShell = profiles.find(profile => profile.profileId === Shells.defaultProfileId)
        const items = [
            { text: qsTr("New terminal"), icon: "terminal",
              programIcon: defaultShell ? defaultShell.icon : "",
              action: function() { WorkspaceController.createSession(projectPath) },
              submenu: profiles.length > 1 ? profiles.map(profile => ({
                  text: profile.name, programIcon: profile.icon,
                  action: function() {
                      WorkspaceController.createSession(projectPath, "", "", profile.profileId)
                  } })) : [] }
        ]

        // Worktrees of a main project: create one, or list an existing one
        // that is not shown yet. Nothing is listed without being asked for.
        if (!isWorktree && Tools.gitAvailable && ProjectRegistry.isGitRepository(projectPath)) {
            const worktreeItems = [
                { text: qsTr("New worktree..."), icon: "plus",
                  enabled: !WorkspaceController.worktreeBusy,
                  action: function() {
                      if (root.hostWindow && root.hostWindow.openNewWorktree)
                          root.hostWindow.openNewWorktree(projectPath)
                  } }
            ]
            const unlisted = ProjectRegistry.unlistedWorktrees(projectPath)
            if (unlisted.length > 0)
                worktreeItems.push({ separator: true })
            for (const worktree of unlisted) {
                const path = worktree.path
                worktreeItems.push({
                    text: worktree.branch.length > 0 && worktree.branch !== worktree.name
                          ? qsTr("Show %1 (%2)").arg(worktree.name).arg(worktree.branch)
                          : qsTr("Show %1").arg(worktree.name),
                    icon: "git-branch",
                    action: function() { ProjectRegistry.addProject(path) } })
            }
            items.push({ text: qsTr("Worktrees"), icon: "git-branch", submenu: worktreeItems })
        }

        // Open in: File Explorer, then the editors Windows recommends for
        // source files (detected, not a fixed list).
        const openItems = [
            { text: qsTr("File Explorer"), icon: "folder-open",
              programIcon: OpenTargets.fileExplorerIcon,
              action: function() { OpenTargets.showInFileExplorer(projectPath) } }
        ].concat(OpenTargets.editors.map(editor => ({
            text: editor.name, icon: "code", programIcon: editor.executable,
            action: function() { OpenTargets.openFolderWith(editor.executable, projectPath) } })))
        items.push({ separator: true },
                   { text: qsTr("Open in"), icon: "external-link", submenu: openItems },
                   { separator: true })

        if (isWorktree) {
            items.push(
                { text: qsTr("Delete worktree..."), icon: "trash", danger: true,
                  enabled: !WorkspaceController.worktreeBusy,
                  action: function() {
                      confirmDialog.openFor(qsTr("Delete worktree"),
                          qsTr("Delete the worktree “%1”? Its folder is removed with git worktree remove; the branch is kept. All its tiles and sessions must be closed first.").arg(projectName),
                          qsTr("Delete"),
                          function() { WorkspaceController.deleteWorktree(projectPath) })
                  } },
                { text: qsTr("Remove from list"), icon: "x",
                  action: function() { WorkspaceController.removeProject(projectPath) } })
        } else {
            items.push(
                { text: qsTr("Remove project"), icon: "trash", danger: true,
                  action: function() {
                      confirmDialog.openFor(qsTr("Remove project"),
                          qsTr("Remove “%1” and the worktrees listed under it from the list? No files are deleted. All their tiles and sessions must be closed first.").arg(projectName),
                          qsTr("Remove"),
                          function() { WorkspaceController.removeProject(projectPath) })
                  } })
        }
        contextMenu.openAt(source, x, y, items)
    }

    function openSessionMenu(source, x, y, sessionIndex, sessionId, sessionName) {
        contextMenu.openAt(source, x, y, [
            { text: qsTr("Rename session"), icon: "pencil",
              action: function() {
                  nameDialog.openFor(qsTr("Rename session"), qsTr("Session name"),
                      qsTr("Rename"),
                      function(name) { SessionModel.renameSession(sessionIndex, name) },
                      sessionName)
              } },
            { separator: true },
            { text: qsTr("Delete session"), icon: "trash", danger: true,
              action: function() {
                  confirmDialog.openFor(qsTr("Delete session"),
                      qsTr("End “%1” and its child processes?").arg(sessionName),
                      qsTr("Delete"),
                      function() { WorkspaceController.removeSession(sessionId) })
              } }
        ])
    }

    function openHostMenu(source, x, y, hostId, hostName) {
        contextMenu.openAt(source, x, y, [
            { text: qsTr("New terminal"), icon: "terminal",
              action: function() { WorkspaceController.createRemoteSession(hostId) } },
            { text: qsTr("Edit host..."), icon: "pencil",
              action: function() { hostDialog.openFor(hostId) } },
            { separator: true },
            { text: qsTr("Remove host"), icon: "trash", danger: true,
              action: function() {
                  confirmDialog.openFor(qsTr("Remove host"),
                      qsTr("Remove “%1”, its saved credentials and its sessions?").arg(hostName),
                      qsTr("Remove"),
                      function() { WorkspaceController.removeRemoteHost(hostId) })
              } }
        ])
    }

    // Section label inside the list column, aligned with the row icon gutter
    // below — no separator line under it.
    component SectionHeader: Item {
        id: header
        property string title: ""
        property string actionIcon: ""
        property string actionTip: ""
        signal actionClicked()
        Layout.fillWidth: true
        Layout.preferredHeight: Theme.sizeMd

        RowLayout {
            anchors.fill: parent
            anchors.leftMargin: Theme.spaceMd
            anchors.rightMargin: Theme.spaceMd
            spacing: Theme.spaceSm

            UiText {
                Layout.fillWidth: true
                text: header.title
                tone: "muted"
                elide: Text.ElideRight
                verticalAlignment: Text.AlignVCenter
            }
            ChromeButton {
                size: "sm"
                kind: "ghost"
                iconName: header.actionIcon
                tip: header.actionTip
                onClicked: header.actionClicked()
            }
        }
    }

    // One terminal session under a project or a remote host.
    component SessionRow: NavRow {
        id: sessionRow
        required property var modelData
        readonly property int sessionIndex: modelData.index
        readonly property string sessionId: modelData.sessionId
        readonly property string sessionName: modelData.name
        readonly property TerminalSession termSession: {
            const _ = root.sessionRevision
            return TerminalManager.session(sessionRow.sessionId)
        }
        readonly property bool isBusy: termSession ? (termSession.running && termSession.active) : false
        readonly property color statusColor: {
            if (!termSession || !termSession.running)
                return Theme.muted
            return sessionRow.isBusy ? Theme.success : Theme.text
        }
        icon: "terminal"
        contentColor: statusColor
        shimmer: isBusy
        label: {
            const _ = root.sessionRevision
            return TerminalManager.sessionTitle(sessionRow.sessionId)
                  || sessionRow.sessionName || qsTr("Terminal")
        }
        selected: SessionModel.activeSessionId === sessionId
        onClicked: mouse => {
            if (mouse.button === Qt.RightButton) {
                root.openSessionMenu(mouseArea, mouse.x, mouse.y,
                    sessionIndex, sessionId, sessionName)
                return
            }
            WorkspaceController.showSession(sessionId)
        }
    }

    // One project row: folder icon, name, and its checked-out branch on the
    // right. Expanded, a main project shows its listed worktrees (through
    // `nested`) and then its own sessions at the bottom. A worktree row
    // starts with a corner glyph in its main project's icon column, and its
    // sessions line up under its folder icon. "No sessions" shows only when
    // the row has nothing under it at all. A project whose folder is gone is
    // dimmed and says so on hover.
    component ProjectNode: Column {
        id: node
        property string name: ""
        property string path: ""
        property string branch: ""
        property bool expanded: false
        property bool available: true
        property bool worktree: false
        property int nestedCount: 0
        default property alias nested: nestedColumn.data
        // Rows under a worktree start at its folder icon.
        readonly property real childIndent: worktree ? Theme.iconMd + Theme.spaceSm : 0
        readonly property var sessions: {
            const revision = root.sessionRevision
            return SessionModel.sessionsForProject(path)
        }
        spacing: Theme.spaceSm

        ListItem {
            id: nodeHeader
            width: parent.width
            expandable: true
            cursorShape: Qt.PointingHandCursor
            onToggled: ProjectRegistry.setExpanded(node.path, !node.expanded)
            onClicked: mouse => {
                if (mouse.button === Qt.RightButton)
                    root.openProjectMenu(mouseArea, mouse.x, mouse.y, node.path, node.name)
            }
            RowLayout {
                anchors.fill: parent
                spacing: Theme.spaceSm
                opacity: node.available ? 1 : Theme.opacityDisabled
                Icon {
                    visible: node.worktree
                    name: "tree-corner"
                    size: Theme.iconMd
                    color: Theme.muted
                }
                FileIcon {
                    Layout.preferredWidth: Theme.iconMd
                    Layout.preferredHeight: Theme.iconMd
                    fileName: node.name
                    isDir: true
                    isRoot: true
                    expanded: node.expanded
                    size: Theme.iconMd
                }
                UiText { Layout.fillWidth: true; text: node.name; elide: Text.ElideRight }
                UiText {
                    visible: node.branch.length > 0
                    // The name keeps at least half the row.
                    Layout.maximumWidth: nodeHeader.width / 2
                    text: node.branch
                    tone: "muted"
                    size: "sm"
                    elide: Text.ElideMiddle
                }
            }
            Tip {
                visible: nodeHeader.hovered && !node.available
                text: qsTr("Folder not found: %1 — right-click to remove it from the list").arg(node.path)
            }
        }

        TreeExpandHost {
            width: parent.width
            expanded: node.expanded
            hasContent: true
            spacing: Theme.spaceSm
            Column {
                id: nestedColumn
                width: parent.width
                spacing: Theme.spaceSm
                visible: node.nestedCount > 0
            }
            Repeater {
                model: node.sessions
                delegate: SessionRow {
                    x: node.childIndent
                    width: node.width - x
                }
            }
            UiText {
                x: node.childIndent
                width: node.width - x
                height: Theme.sizeMd
                leftPadding: Theme.spaceMd
                verticalAlignment: Text.AlignVCenter
                text: qsTr("No sessions")
                tone: "muted"
                visible: node.sessions.length === 0 && node.nestedCount === 0
            }
        }
    }

    AppContextMenu { id: contextMenu }
    NameDialog { id: nameDialog }
    ConfirmDialog { id: confirmDialog }
    RemoteHostDialog { id: hostDialog }

    Connections {
        target: SessionModel
        function onSessionsChanged() { root.sessionRevision += 1 }
        function onActiveSessionIdChanged() { root.sessionRevision += 1 }
    }
    Connections {
        target: TerminalManager
        function onSessionStatesChanged() { root.sessionRevision += 1 }
    }

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: Theme.spaceMd
        spacing: Theme.spaceSm

        SectionHeader {
            title: qsTr("Local")
            actionIcon: "folder-plus"
            actionTip: qsTr("Add project folder...")
            onActionClicked: {
                if (root.hostWindow && root.hostWindow.projectFolderDialog)
                    root.hostWindow.projectFolderDialog.open()
            }
        }

        // The project list, or the empty state while there is no project,
        // hugs its content so the Remote section follows it directly; the
        // list only scrolls once the rail runs out of height.
        Item {
            readonly property real contentHeight: ProjectRegistry.needsInitialProject
                ? noProjects.implicitHeight : projectList.contentHeight
            Layout.fillWidth: true
            Layout.fillHeight: true
            Layout.preferredHeight: contentHeight
            Layout.maximumHeight: contentHeight

            EmptyState {
                id: noProjects
                anchors.fill: parent
                anchors.rightMargin: Theme.spaceMd
                active: ProjectRegistry.needsInitialProject
                icon: "folder-plus"
                title: qsTr("No projects yet")
                subtitle: qsTr("Add a project folder from the Local header")
            }

            // The list bleeds through the column's right margin so its
            // scrollbar sits on the sidebar edge; rows keep the margin.
            TreeListView {
                id: projectList
                anchors.fill: parent
                anchors.rightMargin: -Theme.spaceMd
                spacing: Theme.spaceSm
                model: ProjectRegistry
                visible: !ProjectRegistry.needsInitialProject
                glideHover: true

                delegate: Item {
                    id: projectItem
                    required property int index
                    required property string name
                    required property string path
                    required property bool expanded
                    required property bool available
                    required property string branch
                    // [{name, path, expanded, available, branch}], listed worktrees.
                    required property var worktrees
                    width: projectList.width - Theme.spaceMd
                    height: projectNode.implicitHeight

                    ProjectNode {
                        id: projectNode
                        width: parent.width
                        name: projectItem.name
                        path: projectItem.path
                        expanded: projectItem.expanded
                        available: projectItem.available
                        branch: projectItem.branch
                        nestedCount: projectItem.worktrees.length

                        Repeater {
                            model: projectItem.worktrees
                            delegate: ProjectNode {
                                required property var modelData
                                width: projectNode.width
                                worktree: true
                                name: modelData.name
                                path: modelData.path
                                branch: modelData.branch
                                expanded: modelData.expanded
                                available: modelData.available
                            }
                        }
                    }
                }
            }
        }

        // Divides the Local and Remote sections. The whole Remote section
        // exists only while an OpenSSH client is available.
        PixelLine {
            visible: Tools.sshAvailable
            Layout.fillWidth: true
            Layout.preferredHeight: implicitHeight
            Layout.topMargin: Theme.spaceSm
            Layout.bottomMargin: Theme.spaceSm
            devicePixelRatio: root.renderDpr
            lineColor: Theme.separator
            opacity: Theme.fadeSm
        }

        SectionHeader {
            visible: Tools.sshAvailable
            title: qsTr("Remote")
            actionIcon: "plus"
            actionTip: qsTr("Add SSH host...")
            onActionClicked: hostDialog.openFor("")
        }

        // Saved SSH hosts. The list grows with its content up to a share of
        // the rail so the local projects always keep most of the height.
        Item {
            visible: Tools.sshAvailable
            Layout.fillWidth: true
            Layout.preferredHeight: RemoteHosts.count === 0
                ? addHostRow.height
                : Math.min(hostList.contentHeight, root.height * 0.4) // style-ok: rail share, not a visual value

            ListItem {
                id: addHostRow
                visible: RemoteHosts.count === 0
                width: parent.width - Theme.spaceMd
                cursorShape: Qt.PointingHandCursor
                onClicked: mouse => {
                    if (mouse.button === Qt.LeftButton)
                        hostDialog.openFor("")
                }
                RowLayout {
                    anchors.fill: parent
                    spacing: Theme.spaceSm
                    Icon { name: "plus"; size: Theme.iconMd; color: Theme.muted }
                    UiText {
                        Layout.fillWidth: true
                        text: qsTr("Add an SSH host")
                        tone: "muted"
                        elide: Text.ElideRight
                    }
                }
            }

            TreeListView {
                id: hostList
                anchors.fill: parent
                anchors.rightMargin: -Theme.spaceMd
                spacing: Theme.spaceSm
                model: RemoteHosts
                visible: RemoteHosts.count > 0
                glideHover: true

                delegate: Item {
                    id: hostItem
                    required property string hostId
                    required property string name
                    required property string address
                    required property bool expanded
                    readonly property var sessions: {
                        const revision = root.sessionRevision
                        return SessionModel.sessionsForRemoteHost(hostId)
                    }
                    width: hostList.width - Theme.spaceMd
                    height: hostColumn.implicitHeight

                    Column {
                        id: hostColumn
                        width: parent.width
                        spacing: Theme.spaceSm

                        ListItem {
                            id: hostHeader
                            width: parent.width
                            expandable: true
                            cursorShape: Qt.PointingHandCursor
                            onToggled: RemoteHosts.setExpanded(hostItem.hostId, !hostItem.expanded)
                            onClicked: mouse => {
                                if (mouse.button === Qt.RightButton) {
                                    root.openHostMenu(mouseArea, mouse.x, mouse.y,
                                                      hostItem.hostId, hostItem.name)
                                }
                            }
                            // Saved-session style: double-click a host to connect.
                            onDoubleClicked: mouse => {
                                if (mouse.button === Qt.LeftButton)
                                    WorkspaceController.createRemoteSession(hostItem.hostId)
                            }
                            RowLayout {
                                anchors.fill: parent
                                spacing: Theme.spaceSm
                                Icon { name: "server"; size: Theme.iconMd; color: Theme.text }
                                UiText {
                                    Layout.fillWidth: true
                                    text: hostItem.name
                                    elide: Text.ElideRight
                                }
                            }
                            Tip {
                                visible: hostHeader.hovered
                                text: qsTr("%1 — double-click to open a terminal").arg(hostItem.address)
                            }
                        }

                        TreeExpandHost {
                            width: parent.width
                            expanded: hostItem.expanded
                            hasContent: hostItem.sessions.length > 0
                            spacing: Theme.spaceSm
                            Repeater {
                                model: hostItem.sessions
                                delegate: SessionRow { width: hostItem.width }
                            }
                        }
                    }
                }
            }
        }

        // Takes the leftover height so the app entries stay at the bottom.
        Item {
            Layout.fillWidth: true
            Layout.fillHeight: true
        }

        // App-level entries pinned to the rail's bottom — spacing alone
        // separates them from the list, matching the header row above.
        // The GlideHighlight wrapper shares the rail's one hover pill across
        // the rows (pane tone, not the overlay tone menus use).
        GlideHighlight {
            Layout.fillWidth: true
            Layout.topMargin: Theme.spaceMd
            target: railColumn
            fillColor: Theme.hover
            insetX: Theme.spaceSm
            insetY: PixelGrid.hairline(root.renderDpr)

            Column {
                id: railColumn
                width: parent.width
                spacing: Theme.spaceSm

                ListItem {
                    id: taskRow
                    visible: TaskProgress.count > 0
                    width: parent.width
                    cursorShape: Qt.PointingHandCursor
                    onClicked: {
                        if (root.hostWindow && root.hostWindow.openTaskPopup)
                            root.hostWindow.openTaskPopup(taskRow)
                    }
                    RowLayout {
                        anchors.fill: parent
                        spacing: Theme.spaceSm
                        LoadingSpinner {
                            size: Theme.iconSm
                            active: TaskProgress.primaryOutcome === TaskProgress.Running
                        }
                        UiText {
                            Layout.fillWidth: true
                            size: "sm"
                            tone: "muted"
                            text: TaskProgress.primaryTitle
                            elide: Text.ElideRight
                        }
                    }
                }

                NavRow {
                    id: inboxRow
                    width: parent.width
                    icon: "bell"
                    label: qsTr("Messages")
                    onClicked: {
                        if (root.hostWindow && root.hostWindow.openInbox)
                            root.hostWindow.openInbox(inboxRow)
                    }
                    trailing: Item {
                        readonly property bool show: MessageCenter.unreadCount > 0
                                                     && !MessageCenter.inboxOpen
                        readonly property bool hasError: MessageCenter.unreadErrorCount > 0
                        readonly property string badgeLabel: MessageCenter.unreadCount > 99
                            ? "99+" : String(MessageCenter.unreadCount)
                        readonly property color badgeFill: hasError ? Theme.danger : Theme.accent
                        readonly property color badgeInk: hasError ? Theme.dangerText : Theme.accentText
                        // The digits' ink, relative to the baseline. The circle
                        // grows with the UI font, and the label centres on its
                        // ink: centred by its line box, the empty descent pushes
                        // the digits up, out of a 13 px circle at larger sizes.
                        readonly property rect labelInk: badgeMetrics.tightBoundingRect(badgeLabel)
                        readonly property int badgeH: PixelGrid.snapSize(
                            Math.max(13, Math.ceil(labelInk.height) + Theme.spaceSm), root.renderDpr)
                        readonly property int badgePad: PixelGrid.snapSize(
                            badgeLabel.length > 1 ? Theme.spaceSm : 0, root.renderDpr)

                        visible: show
                        width: show ? Math.max(badgeH,
                            badgeText.implicitWidth + badgePad * 2) : 0
                        height: badgeH

                        FontMetrics {
                            id: badgeMetrics
                            font: badgeText.font
                        }

                        CrispSurface {
                            anchors.fill: parent
                            devicePixelRatio: root.renderDpr
                            radius: height / 2
                            fillTopColor: parent.badgeFill
                            fillMiddleColor: parent.badgeFill
                            fillBottomColor: parent.badgeFill
                            strokeEnabled: false
                        }
                        RenderText {
                            id: badgeText
                            anchors.horizontalCenter: parent.horizontalCenter
                            y: PixelGrid.snap(parent.height / 2 - baselineOffset
                                - parent.labelInk.y - parent.labelInk.height / 2, root.renderDpr)
                            text: parent.badgeLabel
                            color: parent.badgeInk
                            font: Appearance.uiAuxFont(Theme.uiSm, Appearance.fontRevision)
                            horizontalAlignment: Text.AlignHCenter
                        }
                    }
                }

                NavRow {
                    width: parent.width
                    icon: "update"
                    label: qsTr("Updates")
                    shimmer: AppUpdate.busy
                    // Accent marks the indicator, never the label text.
                    trailing: CrispSurface {
                        visible: AppUpdate.updateAvailable
                        width: PixelGrid.snapSize(6, root.renderDpr) // style-ok: status dot, as WorkspaceTab
                        height: width
                        devicePixelRatio: root.renderDpr
                        radius: width / 2
                        fillTopColor: Theme.accent
                        fillMiddleColor: fillTopColor
                        fillBottomColor: fillTopColor
                        strokeEnabled: false
                    }
                    onClicked: {
                        if (root.hostWindow && root.hostWindow.requestUpdate)
                            root.hostWindow.requestUpdate()
                    }
                }

                NavRow {
                    width: parent.width
                    icon: Theme.dark ? "sun" : "moon"
                    label: qsTr("Theme")
                    onClicked: Appearance.themeMode = Appearance.effectiveDark ? "light" : "dark"
                }

                NavRow {
                    width: parent.width
                    icon: "settings"
                    label: qsTr("Settings")
                    onClicked: {
                        if (root.hostWindow)
                            root.hostWindow.page = "settings"
                    }
                }
            }
        }
    }
}
