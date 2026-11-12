pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Layouts
import QtQuick.Window
import SpanTui

Item {
    id: root
    // Tile-internal sibling stack: seams → content.
    readonly property int layerSeam: 30
    readonly property int layerBody: 12
    // Tile elevation inside the workspace delegate stack (above siblings'
    // default 0 so seams and the focus rim compose correctly per tile).
    readonly property int layerTile: 11
    z: root.layerTile
    implicitWidth: 0
    implicitHeight: 0

    property string tileId: ""
    property string tileKind: ""
    property string tileSessionId: ""
    property string tileProjectPath: ""
    property string sessionId: tileSessionId
    property string projectPath: tileProjectPath
    property string tileZone: "center"
    property int tileColumn: 0
    property int tileRow: 0
    property int tileRowSpan: 1
    property bool zoneStacked: false
    property Item canvas: null
    readonly property bool dragging: header.dragging
    property bool closing: false
    property bool geometryReady: false
    property int sessionRevision: 0
    readonly property int lifecycleNormal: 0
    readonly property int lifecycleMoving: 1
    readonly property int lifecycleHidden: 2
    readonly property int lifecycleClosing: 3

    Behavior on x {
        enabled: root.geometryReady && root.canvas
                 && root.canvas.interactionAnimationsEnabled
                 && !root.dragging && !root.canvas.resizeActive
        NumberAnimation { id: xAnim; duration: Theme.durMid; easing.type: Easing.OutCubic }
    }
    Behavior on y {
        enabled: root.geometryReady && root.canvas
                 && root.canvas.interactionAnimationsEnabled
                 && !root.dragging && !root.canvas.resizeActive
        NumberAnimation { id: yAnim; duration: Theme.durMid; easing.type: Easing.OutCubic }
    }
    readonly property bool containerMoving: root.dragging
                                            || (xAnim ? xAnim.running : false)
                                            || (yAnim ? yAnim.running : false)
                                            || (root.canvas ? root.canvas.resizeActive : false)
    readonly property bool effectiveResizeDeferred: (root.hostWindow ? root.hostWindow.panelResizeActive : false)
                                                    || root.containerMoving
    // Terminal presentation receives only this lifecycle state.  X/Y changes
    // are intentionally absent: moving a container is not a terminal resize.
    readonly property int contentLifecycle: root.closing ? root.lifecycleClosing
                                        : !root.visible ? root.lifecycleHidden
                                        : root.effectiveResizeDeferred ? root.lifecycleMoving
                                                                       : root.lifecycleNormal
    readonly property ProjectContext projectContext:
        projectPath.length > 0 ? WorkspaceController.projectContext(projectPath)
                               : null
    readonly property FileTreeModel projectFileTree:
        projectContext ? projectContext.fileTree : null
    readonly property GitRepository projectGit:
        projectContext ? projectContext.gitRepository : null
    readonly property DocumentManager editorDocuments:
        tileKind === "editor" && projectContext
        ? projectContext.editorDocuments(tileId) : null

    signal dragBegan(point globalPos)
    signal dragMoved(point globalPos)
    signal dragEnded(point globalPos)

    readonly property real renderDpr: PixelGrid.safeDpr(Screen.devicePixelRatio)
    readonly property bool isFocused: WorkspaceLayout.focusedTileId === tileId
    readonly property Window hostWindow: Window.window
    readonly property Item windowFocusItem: hostWindow ? hostWindow.activeFocusItem : null
    // The projects rail owns its in-list header row inside Sidebar; other
    // kinds keep the standard PaneHeader strip.
    readonly property bool hasPaneHeader: tileKind !== "projects"
    readonly property bool isRightmostCenterColumn: tileZone === "center"
        && (tileColumn === Math.max(0, WorkspaceLayout.centerColumnWidths.length - 1))
    // A remote tile's header shows the server's figures, not the local
    // ssh.exe process.
    readonly property RemoteSystemMonitor remoteMonitor: {
        const context = projectPath.length > 0 ? WorkspaceController.remoteContextForScope(projectPath) : null
        return context ? context.systemMonitor : null
    }

    function closeThis() {
        if (root.tileKind === "terminal") {
            terminalCloseDialog.openFor(TerminalSettings.terminateOnTileClose,
                                        function(terminateSession) {
                TerminalSettings.terminateOnTileClose = terminateSession
                root.closing = true
                if (!WorkspaceController.closeTerminalTile(root.tileId, terminateSession))
                    root.closing = false
            })
            return
        }
        WorkspaceController.requestCloseTile(root.tileId)
    }

    function activateThis() {
        WorkspaceController.focusTile(root.tileId)
    }

    onWindowFocusItemChanged: {
        let item = root.windowFocusItem
        while (item) {
            if (item === root) {
                root.activateThis()
                return
            }
            item = item.parent
        }
    }

    Component.onCompleted: {
        if (WindowChrome && WindowChrome.addInteractiveItem)
            WindowChrome.addInteractiveItem(root)
        root.geometryReady = true
    }

    Connections {
        target: TerminalManager
        function onSessionStatesChanged() { root.sessionRevision += 1 }
    }
    Connections {
        target: SessionModel
        function onSessionsChanged() { root.sessionRevision += 1 }
    }
    // Flat pane chrome: one solid raised fill under header and body. The
    // content plane (terminal scheme backdrop, document paper) is painted by
    // the body component itself.
    Rectangle {
        anchors.fill: parent
        color: Theme.surfaceRaised
    }

    // Tile-owned seams: right + bottom hairlines drawn inside this tile's
    // bounds, so two panes never paint the same seam twice. The last center
    // column skips its right edge when the dock is open — RightDock draws
    // that zone seam from its own side.
    PixelLine {
        z: root.layerSeam
        vertical: true
        anchors.right: parent.right
        anchors.top: parent.top
        anchors.bottom: parent.bottom
        lineColor: Theme.border
        visible: !(root.isRightmostCenterColumn && root.canvas
                   && root.canvas.rightDockWidth > 0)
    }
    PixelLine {
        z: root.layerSeam
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        lineColor: Theme.border
    }

    ColumnLayout {
        anchors.fill: parent
        z: root.layerBody
        clip: true
        spacing: 0

        PaneHeader {
            id: header
            Layout.fillWidth: true
            visible: root.hasPaneHeader
            dragEnabled: root.tileZone === "center"

            onPressed: root.activateThis()
            onDragBegan: pos => root.dragBegan(pos)
            onDragMoved: pos => root.dragMoved(pos)
            onDragEnded: pos => root.dragEnded(pos)
            onDragCanceled: { }

            leading: Loader {
                anchors.fill: parent
                sourceComponent: {
                    switch (root.tileKind) {
                    case "terminal": return terminalHeader
                    case "editor": return editorHeader
                    default: return null
                    }
                }
            }

            ChromeButton {
                size: "sm"
                kind: "ghost"
                iconName: "x"
                tip: qsTr("Close pane")
                onClicked: root.closeThis()
            }
        }

        PixelLine {
            Layout.fillWidth: true
            Layout.preferredHeight: implicitHeight
            visible: root.hasPaneHeader
            lineColor: Theme.border
        }

        Loader {
            id: bodyLoader
            Layout.fillWidth: true
            Layout.fillHeight: true
            // WorkspaceCanvas has one delegate set per zone.  A tile therefore
            // has hidden mirror delegates which must not create a second view
            // bound to the same terminal session.
            active: root.visible && !root.closing
            sourceComponent: {
                switch (root.tileKind) {
                case "projects": return projectsBody
                case "terminal": return terminalBody
                case "editor": return editorBody
                default: return null
                }
            }
        }
    }

    Component {
        id: terminalHeader
        RowLayout {
            id: terminalHeaderRow
            readonly property TerminalSession termSession: {
                const _ = root.sessionRevision
                return TerminalManager.session(root.sessionId)
            }
            readonly property bool isBusy: termSession ? (termSession.running && termSession.active) : false
            readonly property color statusColor: {
                if (!termSession || !termSession.running)
                    return Theme.muted
                return terminalHeaderRow.isBusy ? Theme.success : Theme.text
            }
            readonly property bool sessionRunning: termSession !== null && termSession.running
            spacing: Theme.spaceSm

            // Right of the title sit two status groups, branch (name and
            // diff stat) and resource readout, then the pane's close button;
            // hairlines split the groups. A narrow header gives way in a
            // fixed order: the readout first, then the diff stat, then the
            // branch name (elided, then icon only, then gone). The budget
            // reads natural widths only, never allocated ones, so it cannot
            // feed back into itself.
            readonly property real titleFloor: Math.min(titleText.implicitWidth, Theme.dialogSm / 4)
            readonly property real dividerSpan: spacing + statusDivider.implicitWidth
            readonly property real baseNeed: statusIcon.implicitWidth + spacing + titleFloor
            readonly property bool branchOn: Boolean(root.projectGit && root.projectGit.available)
            readonly property string branchName: branchOn && root.projectGit.currentBranch.length > 0
                                                 ? root.projectGit.currentBranch : qsTr("HEAD")
            // The chip and the hairline after it.
            readonly property real branchNeed: branchOn ? spacing + terminalBranchButton.naturalWidth + dividerSpan : 0
            readonly property bool diffOn: branchOn && (root.projectGit.diffAdditions > 0
                                                        || root.projectGit.diffDeletions > 0)
            readonly property real diffNeed: diffOn ? spacing + terminalDiffStat.implicitWidth : 0
            // The readout and the hairline after it.
            readonly property real resourceNeed: resourceStrip.active
                                                 ? spacing + resourceStrip.implicitWidth + dividerSpan : 0
            readonly property bool resourcesFit: baseNeed + branchNeed + diffNeed + resourceNeed <= width
            readonly property bool diffShown: diffOn && baseNeed + branchNeed + diffNeed <= width
            // What is left for the chip once everything ahead of it in the
            // order is placed.
            readonly property real branchRoom: width - baseNeed - dividerSpan - spacing
                                               - (diffShown ? diffNeed : 0)
                                               - (resourceStrip.shown ? resourceNeed : 0)
            // The chip elides its name and then keeps only its icon (see
            // ChromeButton.maxWidth); below an icon's room it goes.
            readonly property bool branchShown: branchOn && branchRoom >= terminalBranchButton.controlSize
            readonly property bool branchTruncated: branchRoom < terminalBranchButton.naturalWidth

            Icon {
                id: statusIcon
                name: "terminal"
                size: Theme.iconSm
                color: terminalHeaderRow.statusColor
            }
            UiText {
                id: titleText
                Layout.fillWidth: true
                size: "md"
                color: terminalHeaderRow.statusColor
                shimmer: terminalHeaderRow.isBusy
                text: {
                    const _ = root.sessionRevision
                    return TerminalManager.sessionTitle(root.sessionId)
                          || SessionModel.nameForSessionId(root.sessionId)
                          || qsTr("Terminal")
                }
                elide: Text.ElideRight
                verticalAlignment: Text.AlignVCenter
            }
            // Branch group. Icon and name are one control: a single click
            // target whose hover and pressed fill covers both.
            ChromeButton {
                id: terminalBranchButton
                size: "sm"
                kind: terminalBranchPopup.opened ? "selected" : "ghost"
                iconName: "git-branch"
                visible: terminalHeaderRow.branchShown
                text: terminalHeaderRow.branchName
                maxWidth: Math.max(controlSize, terminalHeaderRow.branchRoom)
                tip: terminalHeaderRow.branchTruncated ? terminalHeaderRow.branchName : ""
                // The header's two popups never stand together: each closes
                // the other (a pinned resource panel included) as it opens.
                // The button takes the press from the header, so it focuses
                // the tile itself, as a press anywhere else on the header does.
                onClicked: {
                    root.activateThis()
                    if (!terminalBranchPopup.opened)
                        resourceStrip.dismiss()
                    terminalBranchPopup.openBelow(this)
                }
            }
            DiffStat {
                id: terminalDiffStat
                Layout.alignment: Qt.AlignVCenter
                visible: terminalHeaderRow.diffShown
                additions: root.projectGit ? root.projectGit.diffAdditions : 0
                deletions: root.projectGit ? root.projectGit.diffDeletions : 0
            }
            PixelLine {
                vertical: true
                Layout.alignment: Qt.AlignVCenter
                Layout.preferredWidth: implicitWidth
                Layout.preferredHeight: Theme.iconSm
                visible: terminalHeaderRow.branchShown && resourceStrip.shown
            }
            // Resource group: slides in and out with tile focus. Once slid
            // out it leaves the layout, so its spacing goes with it.
            Item {
                id: metricsSlot
                clip: true
                visible: Layout.preferredWidth > 0
                Layout.fillHeight: true
                Layout.preferredWidth: resourceStrip.shown ? resourceStrip.implicitWidth : 0
                Behavior on Layout.preferredWidth {
                    NumberAnimation {
                        duration: Theme.durFast
                        easing.type: Easing.OutCubic
                    }
                }
                ResourceStrip {
                    id: resourceStrip
                    anchors.right: parent.right
                    anchors.verticalCenter: parent.verticalCenter
                    server: root.remoteMonitor
                    sessionId: root.sessionId
                    running: terminalHeaderRow.sessionRunning
                    focused: root.isFocused
                    collapsed: active && !terminalHeaderRow.resourcesFit
                    onDetailOpening: terminalBranchPopup.close()
                    opacity: shown ? 1 : 0
                    visible: opacity > 0.01
                    Behavior on opacity {
                        NumberAnimation { duration: Theme.durFast }
                    }
                }
            }
            // Splits the status groups from the close button.
            PixelLine {
                id: statusDivider
                vertical: true
                Layout.alignment: Qt.AlignVCenter
                Layout.preferredWidth: implicitWidth
                Layout.preferredHeight: Theme.iconSm
                visible: terminalHeaderRow.branchShown || resourceStrip.shown
            }
        }
    }
    Component {
        id: editorHeader
        RowLayout {
            spacing: Theme.spaceSm
            Icon {
                name: "file"
                size: Theme.iconSm
                color: root.isFocused ? Theme.accent : Theme.muted
            }
            UiText {
                Layout.fillWidth: true
                size: "md"
                tone: root.isFocused ? "default" : "muted"
                text: qsTr("Editor")
                elide: Text.ElideRight
                verticalAlignment: Text.AlignVCenter
            }
        }
    }
    Component {
        id: projectsBody
        Sidebar { anchors.fill: parent }
    }

    Component {
        id: terminalBody
        TerminalPane {
            anchors.fill: parent
            sessionId: root.sessionId
            color: Theme.terminalBackdrop
            backgroundEnabled: true
            interactionAnimationsEnabled: root.hostWindow
                ? root.hostWindow.interactionAnimationsEnabled : true
            contentLifecycle: root.contentLifecycle
        }
    }

    Component {
        id: editorBody
        PreviewPane {
            anchors.fill: parent
            documentManager: root.editorDocuments
            gitRepository: root.projectGit
            backgroundEnabled: true
            resizeDeferred: root.effectiveResizeDeferred
            onFileLinkActivated: function(target) {
                WorkspaceController.openFileUrl(root.projectPath, target)
            }
        }
    }

    BranchPopup {
        id: terminalBranchPopup
        repository: root.projectGit
        parent: root.hostWindow ? root.hostWindow.contentItem : root
    }
    TerminalCloseDialog { id: terminalCloseDialog }
}
