pragma Translator: "GitHistoryPane"
pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import SpanTui

// Owns only view state: filter, graph index, traced path. History lives in
// GitRepositoryService.
Item {
    id: root
    required property GitRepository repository
    readonly property GitRepository gitRepository: repository
    readonly property GitItemModel historyModel: gitRepository ? gitRepository.history : null

    property string query: ""
    readonly property bool filtering: query.trim().length > 0

    readonly property real renderDpr: PixelGrid.safeDpr(Screen.devicePixelRatio)
    readonly property real hairline: PixelGrid.hairline(renderDpr)

    // One pitch and one column width for the whole list: lanes of adjacent
    // rows meet exactly and the subject column never shifts between rows.
    // Wide histories compress the pitch down to lanePitchMin, then clip.
    readonly property real lanePitchMax: 14
    readonly property real lanePitchMin: 7
    // Outer lane inset: clears the HEAD ring on the first/last lane.
    readonly property real graphPad: 12
    readonly property real graphMaxWidth: Math.max(graphPad * 2,
                                                   Math.min(200, width * 0.32))
    readonly property real nodeRadius: 3.5
    readonly property real nodeGap: 1.5
    readonly property real headRingGap: 2.5
    readonly property real lineWidth: Math.max(1.5, hairline * 2)
    readonly property real pathLineWidth: lineWidth * 1.35
    // Filtering breaks lane continuity (rows collapse), so the graph folds
    // to a single column of nodes.
    readonly property int graphLanes: filtering ? 1 : Math.max(1, laneMax)
    readonly property real lanePitch: graphLanes <= 1 ? lanePitchMax
        : Math.max(lanePitchMin, Math.min(lanePitchMax,
              (graphMaxWidth - graphPad * 2) / (graphLanes - 1)))
    readonly property real graphWidth: PixelGrid.snapSize(
        Math.min(graphMaxWidth, graphPad * 2 + (graphLanes - 1) * lanePitch), renderDpr)

    function laneX(lane: var): real {
        return PixelGrid.snap(graphPad + Number(lane) * lanePitch, renderDpr)
    }
    // A palette entry as stored (Canvas takes it as a style value).
    function graphColor(slot: var): var {
        const palette = Theme.gitGraph
        return palette[Math.abs(Number(slot || 0)) % palette.length]
    }

    // Plain JS maps mutated in place: bindings depend on indexRevision.
    property var rowOfHash: ({})
    property var firstParentOf: ({})
    property int laneMax: 1
    property int indexRevision: 0

    function indexRows(first: int, last: int): void {
        const model = historyModel
        if (!model)
            return
        let lanes = laneMax
        for (let i = first; i <= last; ++i) {
            const item = model.itemAt(i)
            if (!item || !item.hash)
                continue
            rowOfHash[item.hash] = i
            const parents = item.parents || []
            if (parents.length > 0)
                firstParentOf[item.hash] = parents[0]
            lanes = Math.max(lanes, Number(item.laneCount || 1))
        }
        laneMax = lanes
        ++indexRevision
    }
    function rebuildIndex(): void {
        rowOfHash = ({})
        firstParentOf = ({})
        laneMax = 1
        if (historyModel && historyModel.count > 0)
            indexRows(0, historyModel.count - 1)
        else
            ++indexRevision
    }
    onHistoryModelChanged: rebuildIndex()
    Component.onCompleted: rebuildIndex()
    Connections {
        target: root.historyModel
        function onRowsInserted(parent, first, last) {
            // Rows inserted above others shift every row index below them.
            if (last + 1 < root.historyModel.count)
                root.rebuildIndex()
            else
                root.indexRows(first, last)
        }
        function onRowsRemoved(): void { root.rebuildIndex() }
        function onRowsMoved(): void { root.rebuildIndex() }
        function onModelReset(): void { root.rebuildIndex() }
    }

    // The first-parent line of descent of the graph row under the pointer,
    // else of the selected commit. Maps each commit on the path to the row
    // of its path child (-1 for the head), so a through segment can tell it
    // lies between the two — lanes are unique per target commit.
    property string graphHoverHash: ""
    readonly property string selectedHash: gitRepository ? gitRepository.selectedCommit : ""
    readonly property string pathHead: graphHoverHash.length > 0 ? graphHoverHash : selectedHash
    readonly property var pathRows: tracePath(pathHead, indexRevision)
    readonly property bool pathActive: !filtering && pathRows[pathHead] !== undefined

    function tracePath(head: string, revision: int): var {
        const path = {}
        if (!head || rowOfHash[head] === undefined)
            return path
        path[head] = -1
        let hash = head
        for (;;) {
            const parent = firstParentOf[hash]
            if (!parent || path[parent] !== undefined)
                break
            path[parent] = rowOfHash[hash]
            // A parent past the loaded rows still owns the lane below.
            if (rowOfHash[parent] === undefined)
                break
            hash = parent
        }
        return path
    }
    function segmentOnPath(segment: var, row: int, item: var): bool {
        const path = pathRows
        if (segment.kind === "in")
            return path[item.hash] !== undefined && path[item.hash] >= 0
        if (segment.kind === "out")
            return path[item.hash] !== undefined
                   && (item.parents || [])[0] === segment.target
        const child = path[segment.target]
        return child !== undefined && child >= 0 && child < row
    }

    // One card for the list. tipRow trails hoverRow by a grace period, so
    // crossing a row seam swaps the card and the pointer can reach it.
    property Item hoverRow: null
    property Item tipRow: null
    readonly property var tipCommit: tipRow ? tipRow.item : null

    onHoverRowChanged: {
        if (hoverRow) {
            tipGrace.stop()
            tipRow = hoverRow
        } else if (!commitCard.hovered) {
            tipGrace.restart()
        }
    }

    Timer {
        id: tipGrace
        interval: Theme.durMid
        onTriggered: root.tipRow = null
    }

    function commitMatches(item: var): bool {
        const normalized = query.trim().toLowerCase()
        if (normalized.length === 0)
            return true
        return String(item.subject || "").toLowerCase().indexOf(normalized) >= 0
            || String(item.shortHash || "").toLowerCase().indexOf(normalized) >= 0
            || String(item.hash || "").toLowerCase().indexOf(normalized) >= 0
            || String(item.author || "").toLowerCase().indexOf(normalized) >= 0
    }

    function shortDate(value: var): string {
        const date = String(value || "").replace("T", " ")
        return date.length > 16 ? date.substring(0, 16) : date
    }

    function compactDate(value: var): string {
        const date = String(value || "").replace("T", " ")
        return date.length > 10 ? date.substring(0, 10) : date
    }

    // Row decorations: a remote ref pointing where its local branch points
    // folds into the local chip, symbolic remote HEADs drop out (the hover
    // card keeps the full list), and anything past `limit` becomes "+N".
    function rowRefs(decorations: var, limit: int): var {
        const refs = []
        const locals = {}
        const source = decorations || []
        for (let i = 0; i < source.length; ++i) {
            const ref = source[i]
            if (ref.kind === "local") {
                const chip = { label: ref.label, kind: ref.kind, head: ref.head, remotes: [] }
                locals[ref.label] = chip
                refs.push(chip)
            }
        }
        for (let i = 0; i < source.length; ++i) {
            const ref = source[i]
            if (ref.kind === "local")
                continue
            if (ref.kind === "remote") {
                const slash = ref.label.indexOf("/")
                const remote = slash > 0 ? ref.label.substring(0, slash) : ""
                const branch = slash > 0 ? ref.label.substring(slash + 1) : ref.label
                if (branch === "HEAD")
                    continue
                if (locals[branch] !== undefined) {
                    locals[branch].remotes.push(remote)
                    continue
                }
            }
            refs.push(ref)
        }
        // The checked-out branch leads.
        refs.sort((a, b) => (b.head === true) - (a.head === true))
        if (limit <= 0)
            return []
        if (refs.length <= limit)
            return refs
        const shown = refs.slice(0, limit)
        shown.push({ label: "+" + (refs.length - limit), kind: "more", head: false })
        return shown
    }

    function isHeadCommit(item: var): bool {
        const refs = item.decorations || []
        for (let i = 0; i < refs.length; ++i) {
            if (refs[i].head === true)
                return true
        }
        return false
    }

    function showCommit(item: var): void {
        if (!item || !item.hash)
            return
        gitRepository.selectHistoryCommit(item.hash)
    }

    function selectRow(row: int): void {
        if (!historyModel || row < 0 || row >= historyModel.count)
            return
        showCommit(historyModel.itemAt(row))
        historyList.positionViewAtIndex(row, ListView.Contain)
    }

    function goToCommit(hash: string): void {
        const row = rowOfHash[hash]
        if (row === undefined || !historyModel)
            return
        if (!commitMatches(historyModel.itemAt(row)))
            query = ""
        Qt.callLater(function() { root.selectRow(row) })
    }

    function stepSelection(delta: int): void {
        const model = historyModel
        if (!model || model.count === 0 || delta === 0)
            return
        const dir = delta > 0 ? 1 : -1
        const current = rowOfHash[selectedHash]
        let row = current === undefined ? (dir > 0 ? -1 : model.count) : current
        if (!filtering) {
            selectRow(Math.max(0, Math.min(model.count - 1, row + delta)))
            return
        }
        let remaining = Math.abs(delta)
        let target = -1
        for (let i = row + dir; i >= 0 && i < model.count; i += dir) {
            if (!commitMatches(model.itemAt(i)))
                continue
            target = i
            if (--remaining === 0)
                break
        }
        if (target >= 0)
            selectRow(target)
    }

    function handleListKey(event: KeyEvent): void {
        const pageRows = Math.max(1, Math.floor(historyList.height / Theme.sizeMd) - 1)
        const allRows = historyModel ? historyModel.count : 0
        if (event.key === Qt.Key_Up) stepSelection(-1)
        else if (event.key === Qt.Key_Down) stepSelection(1)
        else if (event.key === Qt.Key_PageUp) stepSelection(-pageRows)
        else if (event.key === Qt.Key_PageDown) stepSelection(pageRows)
        else if (event.key === Qt.Key_Home) stepSelection(-allRows)
        else if (event.key === Qt.Key_End) stepSelection(allRows)
        else if (event.key === Qt.Key_Escape && filtering) query = ""
        else if (Shortcuts.matches("git.copyHash", event.key, event.modifiers)
                 && selectedHash.length > 0)
            Clipboard.copyText(selectedHash)
        else
            return
        event.accepted = true
    }

    function openCommitMenu(source: Item, x: real, y: real, item: var): void {
        if (!item || !item.hash)
            return
        const actions = [
            { text: qsTr("Show commit details"),
              icon: "git-commit",
              action: function() { root.showCommit(item) } }
        ]
        const parents = item.parents || []
        if (parents.length > 0)
            actions.push({ separator: true })
        for (let i = 0; i < parents.length; ++i) {
            const parent = parents[i]
            const shortParent = String(parent).substring(0, 7)
            actions.push({
                text: parents.length > 1
                      ? qsTr("Go to parent %1 (%2)").arg(i + 1).arg(shortParent)
                      : qsTr("Go to parent (%1)").arg(shortParent),
                icon: "arrow-down",
                enabled: root.rowOfHash[parent] !== undefined,
                action: function() { root.goToCommit(parent) }
            })
        }
        actions.push({ separator: true })
        actions.push({ text: qsTr("Copy commit hash"), icon: "copy",
                       action: function() { Clipboard.copyText(item.hash) } })
        actions.push({ text: qsTr("Copy commit subject"), icon: "copy",
                       action: function() { Clipboard.copyText(item.subject) } })
        commitMenu.openAt(source, x, y, actions)
    }

    ColumnLayout {
        anchors.fill: parent
        spacing: 0
        visible: Boolean(root.gitRepository) && !root.gitRepository.historyViewLoading

        FilterBar {
            Layout.fillWidth: true
            placeholderText: qsTr("Filter commits")
            text: root.query
            onEdited: text => root.query = text
            // Enter lands on the first match and hands the keys to the list.
            // A selection that still matches is kept.
            onAccepted: {
                const model = root.historyModel
                const current = root.rowOfHash[root.selectedHash]
                if (model && (current === undefined
                              || !root.commitMatches(model.itemAt(current)))) {
                    for (let i = 0; i < model.count; ++i) {
                        if (root.commitMatches(model.itemAt(i))) {
                            root.selectRow(i)
                            break
                        }
                    }
                }
                historyList.forceActiveFocus()
            }
        }

        Item {
            Layout.fillWidth: true
            Layout.fillHeight: true

        // Bleeds into the dock gutter so the scrollbar sits on the dock edge.
        DesktopListView {
            id: historyList
            anchors.fill: parent
            anchors.rightMargin: -Theme.spaceMd
            model: root.historyModel
            clip: true
            boundsBehavior: Flickable.StopAtBounds
            reuseItems: true
            glideHover: true
            keyNavigationEnabled: false
            activeFocusOnTab: true
            Keys.onPressed: event => root.handleListKey(event)
            onAtYEndChanged: {
                if (atYEnd && count > 0 && root.gitRepository)
                    root.gitRepository.loadMoreHistory()
            }

        delegate: Item {
            id: commitRow
            required property var item
            required property int index
            readonly property bool matches: root.commitMatches(item)
            readonly property bool selected: Boolean(item && item.hash === root.selectedHash)
            readonly property int rowH: PixelGrid.snapSize(Theme.sizeMd, root.renderDpr)
            width: historyList.width - Theme.spaceMd
            height: (!root.filtering || matches) ? rowH : 0
            visible: matches
            clip: true
            ListView.onReused: graph.requestPaint()
            ListView.onPooled: {
                if (root.hoverRow === commitRow)
                    root.hoverRow = null
                if (root.tipRow === commitRow)
                    root.tipRow = null
                if (root.graphHoverHash === commitRow.item.hash)
                    root.graphHoverHash = ""
            }
            onSelectedChanged: graph.requestPaint()

            ListItem {
                anchors.fill: parent
                selected: commitRow.selected
                cursorShape: Qt.PointingHandCursor
                onClicked: mouse => {
                    historyList.forceActiveFocus()
                    if (mouse.button === Qt.RightButton)
                        root.openCommitMenu(this, mouse.x, mouse.y, commitRow.item)
                    else
                        root.showCommit(commitRow.item)
                }
                onHoveredChanged: {
                    if (hovered)
                        root.hoverRow = commitRow
                    else if (root.hoverRow === commitRow)
                        root.hoverRow = null
                }
            }

            Canvas {
                id: graph
                anchors.left: parent.left
                anchors.leftMargin: Theme.spaceSm
                anchors.top: parent.top
                anchors.bottom: parent.bottom
                width: root.graphWidth

                readonly property real nodeX: root.filtering
                    ? root.laneX(0) : root.laneX(commitRow.item.nodeLane || 0)
                // HoverHandler never grabs, so the row keeps its hover, click
                // and card.
                readonly property bool nodeHot: graphHover.hovered
                    && Math.abs(graphHover.point.position.x - nodeX)
                       <= root.nodeRadius + root.lanePitchMin

                HoverHandler {
                    id: graphHover
                    onHoveredChanged: {
                        if (hovered)
                            root.graphHoverHash = commitRow.item.hash || ""
                        else if (root.graphHoverHash === commitRow.item.hash)
                            root.graphHoverHash = ""
                    }
                }
                onNodeHotChanged: requestPaint()

                function curve(ctx: var, x0: real, y0: real, x1: real, y1: real): void {
                    ctx.moveTo(x0, y0)
                    if (x0 === x1) {
                        ctx.lineTo(x1, y1)
                        return
                    }
                    const dy = (y1 - y0) * 0.5
                    ctx.bezierCurveTo(x0, y0 + dy, x1, y1 - dy, x1, y1)
                }
                function drawSegment(ctx: var, segment: var, middle: real): void {
                    const fromX = root.laneX(segment.from)
                    const toX = root.laneX(segment.to)
                    ctx.strokeStyle = root.graphColor(segment.color)
                    ctx.beginPath()
                    if (segment.kind === "in") {
                        curve(ctx, fromX, 0, toX, middle)
                    } else if (segment.kind === "out") {
                        curve(ctx, fromX, middle, toX, height)
                    } else {
                        // Lanes change column only in the lower half, like
                        // the node's own out lines: a lane joining this
                        // node's line meets it below the node instead of
                        // seeming to run into it.
                        curve(ctx, fromX, 0, fromX, middle)
                        curve(ctx, fromX, middle, toX, height)
                    }
                    ctx.stroke()
                }
                function circle(ctx: var, x: real, y: real, r: real): void {
                    ctx.beginPath()
                    ctx.arc(x, y, r, 0, Math.PI * 2)
                }

                onPaint: {
                    const ctx = getContext("2d")
                    ctx.reset()
                    const item = commitRow.item
                    if (!item)
                        return
                    const middle = height * 0.5
                    const tracing = root.pathActive
                    ctx.lineCap = "round"

                    // Off-path lines fade but stay legible: a selection is
                    // almost always active.
                    if (!root.filtering) {
                        const segments = item.graphSegments || []
                        const pathSegments = []
                        ctx.globalAlpha = tracing ? Theme.fadeMd : 1
                        ctx.lineWidth = root.lineWidth
                        for (let i = 0; i < segments.length; ++i) {
                            if (tracing && root.segmentOnPath(segments[i], commitRow.index, item))
                                pathSegments.push(segments[i])
                            else
                                drawSegment(ctx, segments[i], middle)
                        }
                        ctx.globalAlpha = 1
                        ctx.lineWidth = root.pathLineWidth
                        for (let i = 0; i < pathSegments.length; ++i)
                            drawSegment(ctx, pathSegments[i], middle)
                    }

                    const color = root.graphColor(item.nodeColor || 0)
                    const merge = (item.parents || []).length > 1
                    const head = root.isHeadCommit(item)
                    const onPath = !tracing || root.pathRows[item.hash] !== undefined
                    const r = root.nodeRadius + ((commitRow.selected || graph.nodeHot) ? 1 : 0)
                    const ringR = r + root.headRingGap
                    ctx.globalAlpha = 1
                    ctx.globalCompositeOperation = "destination-out"
                    circle(ctx, nodeX, middle,
                           head ? ringR + root.lineWidth : r + root.nodeGap)
                    ctx.fill()
                    ctx.globalCompositeOperation = "source-over"
                    ctx.globalAlpha = onPath ? 1 : Theme.fadeLg
                    ctx.fillStyle = color
                    ctx.strokeStyle = color
                    ctx.lineWidth = root.lineWidth
                    if (merge) {
                        circle(ctx, nodeX, middle, r - root.lineWidth * 0.5)
                        ctx.stroke()
                    } else {
                        circle(ctx, nodeX, middle, r)
                        ctx.fill()
                    }
                    if (head) {
                        circle(ctx, nodeX, middle, ringR)
                        ctx.stroke()
                    }
                }
                onWidthChanged: requestPaint()
                onHeightChanged: requestPaint()
                Connections {
                    target: commitRow
                    ignoreUnknownSignals: true
                    function onItemChanged(): void { graph.requestPaint() }
                    function onIndexChanged(): void { graph.requestPaint() }
                }
                Connections {
                    target: root
                    function onRenderDprChanged(): void { graph.requestPaint() }
                    function onLanePitchChanged(): void { graph.requestPaint() }
                    function onPathRowsChanged(): void { graph.requestPaint() }
                    function onPathActiveChanged(): void { graph.requestPaint() }
                    function onFilteringChanged(): void { graph.requestPaint() }
                }
                Connections { target: Theme; function onDarkChanged(): void { graph.requestPaint() } }
            }

            RowLayout {
                anchors.left: graph.right
                anchors.right: parent.right
                anchors.top: parent.top
                anchors.bottom: parent.bottom
                anchors.leftMargin: Theme.spaceSm
                anchors.rightMargin: Theme.spaceMd
                spacing: Theme.spaceMd

                UiText {
                    Layout.fillWidth: true
                    Layout.minimumWidth: 0
                    text: commitRow.item.subject
                    size: "md"
                    emphasis: root.isHeadCommit(commitRow.item)
                    elide: Text.ElideRight
                }
                Repeater {
                    // Chips drop entirely on narrow docks — a half-clipped
                    // chip at the row edge reads as a bug.
                    model: root.rowRefs(commitRow.item.decorations,
                                        root.width >= 600 ? 2 : root.width >= 400 ? 1 : 0)
                    delegate: GitRefBadge {
                        required property var modelData
                        ref: modelData
                    }
                }
                UiText {
                    visible: root.width >= 660
                    Layout.preferredWidth: visible ? 92 : 0
                    Layout.minimumWidth: 0
                    Layout.maximumWidth: visible ? 92 : 0
                    text: commitRow.item.author
                    tone: "muted"
                    size: "sm"
                    elide: Text.ElideRight
                }
                UiText {
                    visible: root.width >= 480
                    Layout.preferredWidth: visible ? implicitWidth : 0
                    Layout.minimumWidth: Layout.preferredWidth
                    Layout.maximumWidth: Layout.preferredWidth
                    text: root.width >= 760
                          ? root.shortDate(commitRow.item.date)
                          : root.compactDate(commitRow.item.date)
                    tone: "muted"
                    size: "sm"
                    elide: Text.ElideNone
                }
            }
        }
        }
        }
    }

    EmptyState {
        anchors.fill: parent
        active: Boolean(root.gitRepository)
                && !root.gitRepository.historyViewLoading
                && !root.gitRepository.commitViewLoading
                && (!root.gitRepository.available || historyList.count === 0)
        icon: "git-branch"
        title: (root.gitRepository && root.gitRepository.available)
               ? qsTr("No commits found")
               : qsTr("This project is not a Git repository")
        subtitle: (root.gitRepository && root.gitRepository.available)
                  ? qsTr("Choose another reference or clear the filter")
                  : qsTr("Initialize Git outside SpanTui, then refresh")
    }

    PaneLoading {
        anchors.fill: parent
        active: Boolean(root.gitRepository && root.gitRepository.historyViewLoading)
        message: qsTr("Loading history…")
    }

    AppContextMenu { id: commitMenu }

    CommitHoverCard {
        id: commitCard
        row: root.tipRow
        commit: root.tipCommit
        repository: root.gitRepository
        fallbackParent: root
        visible: Boolean(root.tipCommit && root.tipCommit.hash) && !commitMenu.opened
        onHoveredChanged: {
            if (hovered)
                tipGrace.stop()
            else if (!root.hoverRow)
                tipGrace.restart()
        }
        onShowDetailsRequested: commit => {
            root.tipRow = null
            root.showCommit(commit)
        }
    }
}
