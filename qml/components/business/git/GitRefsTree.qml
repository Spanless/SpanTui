pragma Translator: "GitHistoryPane"
pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import SpanTui

// Body of the title-bar branch popup: a filter field with a refresh button
// above the repository's local branches, other worktrees, remote branches
// and tags. Sections without rows are not drawn. Nothing here changes the
// repository.
//
// Branch-centred, one line per row, marks instead of words: a local branch
// shows how far it is ahead (not pushed) and behind (not pulled) of its
// upstream, as of the last fetch, or a mark for none / gone. Once the
// repository has linked worktrees, a branch checked out in one ends in a
// folder mark (a dot for uncommitted changes) and carries its worktree
// actions; names and paths are in the row's tip. Worktrees on no local
// branch (detached, bare) are listed apart under "Other worktrees".
//
// A left click on a branch checked out in another worktree opens that
// worktree as a project; on a worktree row, likewise. Every other click, and
// a right click everywhere, opens the row's context menu. The popup asks to
// close (dismissRequested) once it hands the user elsewhere.
Item {
    id: root
    required property GitRepository repository
    readonly property GitRepository gitRepository: repository

    property string query: ""
    readonly property bool filtering: query.trim().length > 0
    property bool localExpanded: true
    property bool otherWorktreeExpanded: true
    property bool remoteExpanded: true
    property bool tagExpanded: true
    property var remoteGroupExpansion: ({})

    signal dismissRequested()

    // Rows climb past refsColumn to this root for the glide context —
    // declared, not injected: QML cannot add dynamic properties at runtime.
    property var glideContext: refsGlide.glideEnabled ? refsGlide : null

    // GitItemModel updates rows in place via dataChanged. The derived arrays
    // below read itemAt(), which QML cannot observe, so every mutation bumps
    // these revisions to force the arrays to rebuild.
    property int branchesRevision: 0
    property int worktreesRevision: 0
    Connections {
        target: root.gitRepository ? root.gitRepository.branches : null
        ignoreUnknownSignals: true
        function onDataChanged() { root.branchesRevision += 1 }
        function onModelReset() { root.branchesRevision += 1 }
        function onRowsInserted() { root.branchesRevision += 1 }
        function onRowsRemoved() { root.branchesRevision += 1 }
    }
    Connections {
        target: root.gitRepository ? root.gitRepository.worktrees : null
        ignoreUnknownSignals: true
        function onDataChanged() { root.worktreesRevision += 1 }
        function onModelReset() { root.worktreesRevision += 1 }
        function onRowsInserted() { root.worktreesRevision += 1 }
        function onRowsRemoved() { root.worktreesRevision += 1 }
    }

    readonly property real renderDpr: PixelGrid.safeDpr(Screen.devicePixelRatio)

    // Worktree path -> uncommitted changes to tracked files; a missing path
    // is unknown (the probe has not reached it, or skipped it).
    readonly property var worktreeChanges: gitRepository ? gitRepository.worktreeChanges : ({})

    // Every worktree, unfiltered. A lone main worktree is just the
    // repository itself: worktree lines appear once linked ones exist.
    readonly property var allWorktrees: {
        const _worktrees = root.worktreesRevision
        const model = gitRepository ? gitRepository.worktrees : null
        const count = model ? model.count : 0
        const list = []
        if (count < 2)
            return list
        for (let i = 0; i < count; ++i) {
            const it = model.itemAt(i)
            if (it)
                list.push(it)
        }
        return list
    }

    // Branch → the worktree that has it checked out, from the unfiltered
    // list: a filter must not hide where a branch lives.
    readonly property var worktreeByBranch: {
        const map = ({})
        for (const it of root.allWorktrees) {
            if (!it.bare && !it.detached && String(it.branch || "").length > 0)
                map[it.branch] = it
        }
        return map
    }

    readonly property var localBranchNames: {
        const _branches = root.branchesRevision
        const names = ({})
        const model = gitRepository ? gitRepository.branches : null
        const count = model ? model.count : 0
        for (let i = 0; i < count; ++i) {
            const it = model.itemAt(i)
            if (it && it.kind === "local")
                names[it.name] = true
        }
        return names
    }

    readonly property var localBranches: {
        const _q = root.query
        const _branches = root.branchesRevision
        const _trees = root.worktreeByBranch
        return root.collectRefs("local")
    }

    // Worktrees no local branch row carries: detached, bare, or on a branch
    // the ref list does not have (yet).
    readonly property var otherWorktrees: {
        const _q = root.query
        const names = root.localBranchNames
        const list = []
        for (const it of root.allWorktrees) {
            const branch = String(it.branch || "")
            const onLocalBranch = !it.bare && !it.detached && branch.length > 0 && names[branch] === true
            if (!onLocalBranch && root.worktreeMatches(it))
                list.push(it)
        }
        return list
    }

    // Grouped by remote in order of first appearance: the refs arrive sorted
    // by date, so two remotes would otherwise interleave their headers.
    readonly property var remoteBranches: {
        const _q = root.query
        const _branches = root.branchesRevision
        const groups = []
        const byGroup = ({})
        for (const item of root.collectRefs("remote")) {
            const group = String(item.group)
            if (byGroup[group] === undefined) {
                byGroup[group] = []
                groups.push(group)
            }
            byGroup[group].push(item)
        }
        let list = []
        for (const group of groups)
            list = list.concat(byGroup[group])
        return list
    }

    readonly property var tagBranches: {
        const _q = root.query
        const _branches = root.branchesRevision
        return root.collectRefs("tag")
    }

    readonly property bool hasLocal: localBranches.length > 0
    readonly property bool hasOtherWorktrees: otherWorktrees.length > 0
    readonly property bool hasRemote: remoteBranches.length > 0
    readonly property bool hasTags: tagBranches.length > 0
    readonly property bool hasRows: hasLocal || hasOtherWorktrees || hasRemote || hasTags

    readonly property bool refreshing: gitRepository ? gitRepository.refsRefreshing : false
    // A read that lands in a few milliseconds would flash the spinner: it
    // stays up for at least one slow beat.
    property bool refreshShown: refreshing

    implicitHeight: refsLayout.implicitHeight

    onRefreshingChanged: {
        if (refreshing) {
            refreshShown = true
            refreshHold.restart()
        } else if (!refreshHold.running) {
            refreshShown = false
        }
    }

    Timer {
        id: refreshHold
        interval: Theme.durSlow
        onTriggered: {
            if (!root.refreshing)
                root.refreshShown = false
        }
    }

    function clearFilterAndFocus() {
        query = ""
        refFilter.text = ""
        refFilter.field.forceActiveFocus()
    }

    function collectRefs(kind) {
        const list = []
        const model = gitRepository ? gitRepository.branches : null
        const count = model ? model.count : 0
        for (let i = 0; i < count; ++i) {
            const it = model.itemAt(i)
            if (it && root.refMatches(it, kind))
                list.push(it)
        }
        return list
    }

    function textMatches(fields) {
        const normalized = query.trim().toLowerCase()
        if (normalized.length === 0)
            return true
        for (const field of fields) {
            if (String(field || "").toLowerCase().indexOf(normalized) >= 0)
                return true
        }
        return false
    }

    // A local branch also matches on the worktree it is checked out in.
    function refMatches(item, kind) {
        if (item.kind !== kind)
            return false
        const tree = kind === "local" ? worktreeFor(item.name) : null
        return textMatches(tree ? [item.name, tree.name, tree.displayPath] : [item.name])
    }

    function worktreeMatches(item) {
        return textMatches([item.name, item.branch, item.displayPath])
    }

    function worktreeFor(branch) {
        const tree = worktreeByBranch[String(branch)]
        return tree ? tree : null
    }

    function remoteGroupIsExpanded(group) {
        return remoteGroupExpansion[String(group)] !== false
    }

    function toggleRemoteGroup(group) {
        const key = String(group)
        const next = Object.assign({}, remoteGroupExpansion)
        next[key] = !remoteGroupIsExpanded(key)
        remoteGroupExpansion = next
    }

    function remoteLeafName(item) {
        const name = String(item.name || "")
        const group = String(item.group || "")
        const prefix = group + "/"
        return group.length > 0 && name.startsWith(prefix)
                ? name.substring(prefix.length) : name
    }

    function worktreeBranchLabel(item) {
        if (item.bare)
            return qsTr("bare")
        if (item.detached || String(item.branch || "").length === 0)
            return String(item.head || "").length > 0 ? qsTr("detached %1").arg(item.head)
                                                      : qsTr("detached")
        return item.branch
    }

    // true / false, or undefined while unknown.
    function worktreeDirty(item) {
        const changes = root.worktreeChanges
        return item ? changes[item.path] : undefined
    }

    function canOpenWorktree(item) {
        return item !== null && !item.bare && !item.prunable && String(item.path || "").length > 0
    }

    // One Tip for the whole list; the hovered row publishes its anchor.
    property Item tipRow: null
    property string tipText: ""
    function hoverTip(row, hovered, text) {
        if (hovered && text.length > 0) {
            tipRow = row
            tipText = text
        } else if (tipRow === row) {
            tipRow = null
        }
    }

    function trackingTip(item) {
        const upstream = String(item.upstream || "")
        if (!item.hasUpstream)
            return qsTr("No upstream branch: never pushed, or not set to track one.")
        if (item.upstreamGone)
            return qsTr("Upstream %1 is gone: deleted on the remote and pruned here.").arg(upstream)
        const counts = item.ahead > 0 || item.behind > 0
            ? qsTr("Tracks %1: ↑%2 not pushed, ↓%3 not pulled.").arg(upstream).arg(item.ahead).arg(item.behind)
            : qsTr("Up to date with %1.").arg(upstream)
        return counts + "\n" + qsTr("As of the last fetch; this popup never fetches.")
    }

    function worktreeTip(item) {
        const lines = [String(item.displayPath || "")]
        if (item.main)
            lines.push(qsTr("Main worktree"))
        if (item.locked)
            lines.push(String(item.lockReason || "").length > 0 ? qsTr("Locked: %1").arg(item.lockReason)
                                                                : qsTr("Locked"))
        if (item.prunable)
            lines.push(qsTr("Its folder is missing."))
        if (worktreeDirty(item) === true)
            lines.push(qsTr("Uncommitted changes to tracked files."))
        return lines.join("\n")
    }

    // Upstream state, then where it is checked out (the worktree's path and
    // marks); the current branch needs no line, its row is selected.
    function localTip(item, tree) {
        const lines = [trackingTip(item)]
        if (tree)
            lines.push(worktreeTip(tree))
        return lines.join("\n")
    }

    function copyText(text) {
        if (!text)
            return
        Clipboard.copyText(String(text))
        dismissRequested()
    }

    // As a project: its last session, or a new terminal (newTerminal: always
    // a new one).
    function openWorktree(item, newTerminal) {
        if (!canOpenWorktree(item))
            return
        if (WorkspaceController.openProjectFolder(item.path, newTerminal === true).length > 0)
            dismissRequested()
    }

    function revealPath(path) {
        const text = String(path)
        Qt.openUrlExternally(text.startsWith("file:") ? text
                                                      : "file:///" + text.replace(/\\/g, "/"))
        dismissRequested()
    }

    function worktreeActions(tree) {
        return [
            { text: qsTr("Open as project"), icon: "folder-open",
              enabled: canOpenWorktree(tree) && !tree.current,
              action: function() { root.openWorktree(tree, false) } },
            { text: qsTr("Open in new terminal"), icon: "terminal",
              enabled: canOpenWorktree(tree),
              action: function() { root.openWorktree(tree, true) } },
            { text: qsTr("Open in File Explorer"), icon: "external-link",
              enabled: !tree.prunable,
              action: function() { root.revealPath(tree.path) } },
            { text: qsTr("Copy path"), icon: "copy",
              action: function() { root.copyText(tree.displayPath) } }
        ]
    }

    function commitAction(hash) {
        const id = String(hash || "")
        return { text: qsTr("Copy commit hash"), icon: "git-commit", enabled: id.length > 0,
                 action: function() { root.copyText(id) } }
    }

    function openLocalMenu(source, x, y, item, tree) {
        const name = String(item.name)
        let actions = []
        if (tree)
            actions = worktreeActions(tree).concat([{ separator: true }])
        actions.push({ text: qsTr("Copy branch name"), icon: "copy",
                       action: function() { root.copyText(name) } })
        actions.push(commitAction(item.objectId))
        contextMenu.openAt(source, x, y, actions)
    }

    function openOtherWorktreeMenu(source, x, y, item) {
        contextMenu.openAt(source, x, y,
                           worktreeActions(item).concat([{ separator: true },
                                                         commitAction(item.headId)]))
    }

    function openRemoteMenu(source, x, y, item) {
        const name = String(item.name)
        contextMenu.openAt(source, x, y, [
            { text: qsTr("Copy branch name"), icon: "copy",
              action: function() { root.copyText(name) } },
            commitAction(item.objectId)
        ])
    }

    function openTagMenu(source, x, y, item) {
        const name = String(item.name)
        contextMenu.openAt(source, x, y, [
            { text: qsTr("Copy tag name"), icon: "copy",
              action: function() { root.copyText(name) } },
            // An annotated tag's own object is not the commit.
            commitAction(String(item.commitId || "").length > 0 ? item.commitId : item.objectId)
        ])
    }

    function localClicked(row, mouse, item, tree) {
        if (mouse.button === Qt.LeftButton && tree && !tree.current && canOpenWorktree(tree))
            openWorktree(tree, false)
        else
            openLocalMenu(row, mouse.x, mouse.y, item, tree)
    }

    function otherWorktreeClicked(row, mouse, item) {
        if (mouse.button === Qt.LeftButton && !item.current && canOpenWorktree(item))
            openWorktree(item, false)
        else
            openOtherWorktreeMenu(row, mouse.x, mouse.y, item)
    }

    AppContextMenu { id: contextMenu }

    // Left of the popup, level with the row: below it, the multi-line tip
    // covered the next rows, and a click on one landed on the tip.
    Tip {
        parent: root.tipRow ? root.tipRow : root
        visible: root.tipRow !== null && !contextMenu.visible
        text: root.tipText
        wrap: true
        maxTextWidth: Theme.dialogSm
        x: -width - Theme.spaceMd
        y: 0
    }

    // Dot of a worktree with uncommitted changes to tracked files.
    component DirtyDot: Rectangle {
        Layout.alignment: Qt.AlignVCenter
        implicitWidth: Theme.spaceSm
        implicitHeight: Theme.spaceSm
        radius: width / 2
        color: Theme.gitModified
    }

    // Trailing mark of a branch row checked out in a worktree: a folder
    // (accent for the worktree this pane is in, danger when its folder is
    // missing) and a dot for uncommitted changes. Its name and path are in
    // the row's tip; a left click opens it.
    component WorktreeMark: RowLayout {
        id: mark
        property var tree: null
        spacing: Theme.spaceSm
        Icon {
            Layout.alignment: Qt.AlignVCenter
            name: "folder"
            size: Theme.iconSm
            color: !mark.tree ? Theme.muted
                 : mark.tree.prunable ? Theme.danger
                 : mark.tree.current ? Theme.accent : Theme.muted
        }
        DirtyDot {
            visible: root.worktreeDirty(mark.tree) === true
        }
    }

    ColumnLayout {
        id: refsLayout
        anchors.fill: parent
        spacing: 0

        RowLayout {
            Layout.fillWidth: true
            spacing: 0

            FilterBar {
                id: refFilter
                Layout.fillWidth: true
                placeholderText: qsTr("Filter")
                text: root.query
                onEdited: text => root.query = text
            }

            // The button and its spinner trade places in one slot, so the
            // field never changes width while refreshing.
            Item {
                Layout.alignment: Qt.AlignVCenter
                Layout.rightMargin: Theme.spaceSm
                implicitWidth: refreshButton.implicitWidth
                implicitHeight: refreshButton.implicitHeight

                ChromeButton {
                    id: refreshButton
                    kind: "ghost"
                    iconName: "refresh"
                    tip: qsTr("Refresh branches, tags and worktrees")
                    visible: !root.refreshShown
                    onClicked: root.gitRepository.refreshRefs()
                }

                LoadingSpinner {
                    anchors.centerIn: parent
                    active: root.refreshShown
                    visible: root.refreshShown
                }
            }
        }

        EmptyState {
            Layout.fillWidth: true
            active: !root.hasRows
            icon: root.filtering ? "search" : "git-branch"
            title: root.filtering ? qsTr("No matches") : qsTr("No branches yet")
            subtitle: root.filtering ? qsTr("Try another name, or clear the filter.")
                                     : qsTr("They appear after the first commit.")
        }

        DesktopScrollView {
            id: refsView
            visible: root.hasRows
            Layout.fillWidth: true
            Layout.fillHeight: true
            Layout.preferredHeight: Math.max(Theme.sizeMd * 3,
                                             refsColumn.implicitHeight
                                             + topPadding + bottomPadding)
            // The popup's rounded lower corners sit under this edge: keeping
            // the view clear of it lets rows and bar ends stay inside the curve.
            Layout.bottomMargin: Theme.spaceSm
            clip: true
            // The viewport owns the width.
            contentWidth: availableWidth
            scrollBarEndInset: Theme.radiusLg
            ScrollBar.horizontal.policy: ScrollBar.AlwaysOff

            Column {
                id: refsColumn
                width: refsView.availableWidth

                // Local branches, each with the worktree it is checked out in
                RefsSectionHeader {
                    width: parent.width
                    visible: root.hasLocal
                    text: qsTr("Local")
                    expanded: root.localExpanded || root.filtering
                    onToggled: root.localExpanded = !root.localExpanded
                }
                TreeExpandHost {
                    visible: root.hasLocal
                    expanded: root.localExpanded || root.filtering
                    hasContent: localRepeater.count > 0
                    Repeater {
                        id: localRepeater
                        model: root.localBranches
                        delegate: ListItem {
                            id: localRefRow
                            required property var modelData
                            readonly property var item: modelData
                            readonly property var tree: root.worktreeFor(item.name)
                            readonly property bool tracking: item.hasUpstream && !item.upstreamGone
                            width: refsColumn.width
                            selected: localRefRow.item.current
                            onClicked: mouse => root.localClicked(
                                           localRefRow, mouse, localRefRow.item, localRefRow.tree)
                            onHoveredChanged: root.hoverTip(
                                localRefRow, hovered, root.localTip(localRefRow.item, localRefRow.tree))
                            // One line: name, upstream marks, then the
                            // worktree mark, every row the same height.
                            RowLayout {
                                anchors.fill: parent
                                anchors.leftMargin: Theme.treeDepthStep
                                anchors.rightMargin: Theme.spaceMd
                                spacing: Theme.spaceSm
                                z: Theme.zRaised
                                Icon {
                                    Layout.alignment: Qt.AlignVCenter
                                    name: "git-branch"
                                    size: Theme.iconMd
                                    color: localRefRow.item.current ? Theme.accent : Theme.muted
                                }
                                UiText {
                                    Layout.fillWidth: true
                                    Layout.minimumWidth: 0
                                    text: localRefRow.item.name
                                    size: "md"
                                    emphasis: localRefRow.item.current
                                    elide: Text.ElideRight
                                }
                                // Ahead: commits not pushed; behind: not pulled.
                                MonoText {
                                    Layout.alignment: Qt.AlignVCenter
                                    visible: localRefRow.tracking && localRefRow.item.ahead > 0
                                    text: "↑" + localRefRow.item.ahead
                                    px: Theme.uiSm
                                    color: Theme.gitAdded
                                }
                                MonoText {
                                    Layout.alignment: Qt.AlignVCenter
                                    visible: localRefRow.tracking && localRefRow.item.behind > 0
                                    text: "↓" + localRefRow.item.behind
                                    px: Theme.uiSm
                                    color: Theme.gitModified
                                }
                                // No upstream: never pushed (muted up arrow);
                                // upstream deleted: warning. Words in the tip.
                                Icon {
                                    Layout.alignment: Qt.AlignVCenter
                                    visible: !localRefRow.tracking
                                    name: localRefRow.item.upstreamGone === true ? "alert" : "arrow-up"
                                    size: Theme.iconSm
                                    color: localRefRow.item.upstreamGone === true ? Theme.warning : Theme.muted
                                }
                                WorktreeMark {
                                    visible: localRefRow.tree !== null
                                    tree: localRefRow.tree
                                }
                            }
                        }
                    }
                }

                // Worktrees no local branch row carries
                Item {
                    width: parent.width
                    height: Theme.spaceSm
                    visible: root.hasOtherWorktrees && root.hasLocal
                }
                RefsSectionHeader {
                    width: parent.width
                    visible: root.hasOtherWorktrees
                    text: qsTr("Other worktrees")
                    expanded: root.otherWorktreeExpanded || root.filtering
                    onToggled: root.otherWorktreeExpanded = !root.otherWorktreeExpanded
                }
                TreeExpandHost {
                    visible: root.hasOtherWorktrees
                    expanded: root.otherWorktreeExpanded || root.filtering
                    hasContent: otherWorktreeRepeater.count > 0
                    Repeater {
                        id: otherWorktreeRepeater
                        model: root.otherWorktrees
                        delegate: ListItem {
                            id: worktreeRow
                            required property var modelData
                            readonly property var item: modelData
                            width: refsColumn.width
                            selected: worktreeRow.item.current
                            onClicked: mouse => root.otherWorktreeClicked(
                                           worktreeRow, mouse, worktreeRow.item)
                            onHoveredChanged: root.hoverTip(
                                worktreeRow, hovered, root.worktreeTip(worktreeRow.item))
                            // One line, like a branch row: the folder, what
                            // it has checked out, the uncommitted dot.
                            RowLayout {
                                anchors.fill: parent
                                anchors.leftMargin: Theme.treeDepthStep
                                anchors.rightMargin: Theme.spaceMd
                                spacing: Theme.spaceSm
                                z: Theme.zRaised
                                Icon {
                                    Layout.alignment: Qt.AlignVCenter
                                    name: "folder"
                                    size: Theme.iconMd
                                    color: worktreeRow.item.prunable ? Theme.danger
                                         : worktreeRow.item.current ? Theme.accent : Theme.muted
                                }
                                UiText {
                                    Layout.fillWidth: true
                                    Layout.minimumWidth: 0
                                    text: String(worktreeRow.item.name || worktreeRow.item.displayPath || "")
                                    size: "md"
                                    emphasis: worktreeRow.item.current
                                    elide: Text.ElideRight
                                }
                                UiText {
                                    Layout.maximumWidth: Math.round(worktreeRow.width / 2)
                                    text: root.worktreeBranchLabel(worktreeRow.item)
                                    size: "sm"
                                    tone: "muted"
                                    elide: Text.ElideRight
                                }
                                DirtyDot {
                                    visible: root.worktreeDirty(worktreeRow.item) === true
                                }
                            }
                        }
                    }
                }

                // Remote
                Item {
                    width: parent.width
                    height: Theme.spaceSm
                    visible: root.hasRemote && (root.hasLocal || root.hasOtherWorktrees)
                }
                RefsSectionHeader {
                    width: parent.width
                    visible: root.hasRemote
                    text: qsTr("Remote")
                    expanded: root.remoteExpanded || root.filtering
                    onToggled: root.remoteExpanded = !root.remoteExpanded
                }
                TreeExpandHost {
                    visible: root.hasRemote
                    expanded: root.remoteExpanded || root.filtering
                    hasContent: remoteRepeater.count > 0
                    Repeater {
                        id: remoteRepeater
                        model: root.remoteBranches
                        delegate: Column {
                            id: remoteRefRow
                            required property int index
                            required property var modelData
                            readonly property var item: modelData
                            readonly property bool firstInGroup: {
                                if (index === 0)
                                    return true
                                const prev = remoteRepeater.model[index - 1]
                                return !prev || String(prev.group) !== String(item.group)
                            }
                            readonly property bool groupOpen: root.filtering
                                || root.remoteGroupIsExpanded(item.group)
                            width: refsColumn.width
                            TreeGroupHeader {
                                width: parent.width
                                visible: remoteRefRow.firstInGroup
                                height: visible ? Theme.sizeMd : 0
                                text: String(remoteRefRow.item.group || qsTr("Remote"))
                                iconName: "git-remote"
                                depth: 1
                                expanded: remoteRefRow.groupOpen
                                onToggled: root.toggleRemoteGroup(remoteRefRow.item.group)
                            }
                            TreeExpandHost {
                                expanded: remoteRefRow.groupOpen
                                ListItem {
                                    id: remoteItem
                                    width: parent.width
                                    onClicked: mouse => root.openRemoteMenu(
                                                   remoteItem, mouse.x, mouse.y, remoteRefRow.item)
                                    RowLayout {
                                        anchors.fill: parent
                                        anchors.leftMargin: Theme.treeDepthStep * 2
                                        spacing: Theme.spaceSm
                                        z: Theme.zRaised
                                        Icon { name: "git-branch"; size: Theme.iconMd; color: Theme.gitModified }
                                        UiText { Layout.fillWidth: true; Layout.minimumWidth: 0; text: root.remoteLeafName(remoteRefRow.item); size: "md"; elide: Text.ElideRight }
                                    }
                                }
                            }
                        }
                    }
                }

                // Tags
                Item {
                    width: parent.width
                    height: Theme.spaceSm
                    visible: root.hasTags && (root.hasLocal || root.hasOtherWorktrees || root.hasRemote)
                }
                RefsSectionHeader {
                    width: parent.width
                    visible: root.hasTags
                    text: qsTr("Tags")
                    expanded: root.tagExpanded || root.filtering
                    onToggled: root.tagExpanded = !root.tagExpanded
                }
                TreeExpandHost {
                    visible: root.hasTags
                    expanded: root.tagExpanded || root.filtering
                    hasContent: tagRepeater.count > 0
                    Repeater {
                        id: tagRepeater
                        model: root.tagBranches
                        delegate: ListItem {
                            id: tagRefRow
                            required property var modelData
                            readonly property var item: modelData
                            width: refsColumn.width
                            onClicked: mouse => root.openTagMenu(
                                           tagRefRow, mouse.x, mouse.y, tagRefRow.item)
                            RowLayout {
                                anchors.fill: parent
                                anchors.leftMargin: Theme.treeDepthStep
                                spacing: Theme.spaceSm
                                z: Theme.zRaised
                                Icon { name: "tag"; size: Theme.iconMd; color: Theme.gitRenamed }
                                UiText { Layout.fillWidth: true; Layout.minimumWidth: 0; text: tagRefRow.item.name; size: "md"; elide: Text.ElideRight }
                            }
                        }
                    }
                }
            }

            // Shared hover pill riding the scroll content; rows climb to
            // this root for the declared glideContext forwarder.
            GlideHighlight {
                id: refsGlide
                parent: refsColumn.parent
                target: refsColumn
                fillColor: Theme.hover
                insetX: Theme.spaceSm
                insetY: PixelGrid.hairline(root.renderDpr)
                scrollOffset: refsView.contentItem
                              ? refsView.contentItem.contentY : 0
            }
        }
    }
}
