import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import SpanTui

// Worktree changes or a selected commit. A diff body is fetched only while
// its row is expanded and alive.
Item {
    id: root
    required property GitRepository repository
    readonly property GitRepository gitRepository: repository
    // "changes" (worktree) or "commit".
    property string scope: "changes"

    property GitItemModel model: null
    property bool loading: false
    property bool allowWorkspaceOpen: true
    property string emptyText: qsTr("No changed files")
    property string mode: "unified"
    property bool grouped: false
    property int modelRevision: 0
    // Keyed by path here, not in delegates, which are recycled.
    property var expandedPaths: ({})
    property var collapsedDirs: ({})
    // Scrollbar drag in progress; see fileList.
    property bool fastScroll: false

    // Row order, per-directory totals and the widest stat, once per revision.
    readonly property var summary: {
        const revision = root.modelRevision
        const entries = []
        const groups = ({})
        let additions = 0
        let deletions = 0
        // Widest stat as mono glyph counts: one side alone, or both sides
        // (which also pay DiffStat's inner spacing).
        let single = 0
        let pair = 0
        const measure = (adds, dels) => {
            const plus = adds > 0 ? String(adds).length + 1 : 0
            const minus = dels > 0 ? String(dels).length + 1 : 0
            if (plus > 0 && minus > 0)
                pair = Math.max(pair, plus + minus)
            else
                single = Math.max(single, plus + minus)
        }
        const count = root.model ? root.model.count : 0
        for (let index = 0; index < count; ++index) {
            const item = root.model.itemAt(index)
            if (!item || !item.path)
                continue
            const slash = item.path.lastIndexOf("/")
            const dir = slash > 0 ? item.path.substring(0, slash) : ""
            const adds = Number(item.additions || 0)
            const dels = Number(item.deletions || 0)
            entries.push({ index: index, dir: dir, path: item.path, item: item })
            measure(adds, dels)
            if (!groups[dir])
                groups[dir] = { additions: 0, deletions: 0 }
            groups[dir].additions += adds
            groups[dir].deletions += dels
            additions += adds
            deletions += dels
        }
        // Grouped: root files first, then directories by path; files by name
        // inside; header sums join the stat measure. Flat keeps model order.
        if (root.grouped) {
            entries.sort((a, b) => {
                const left = a.dir === b.dir ? a.path : a.dir
                const right = a.dir === b.dir ? b.path : b.dir
                return left < right ? -1 : left > right ? 1 : 0
            })
            for (const dir in groups)
                measure(groups[dir].additions, groups[dir].deletions)
        }
        return {
            entries: entries,
            groups: groups,
            additions: additions,
            deletions: deletions,
            statSingleChars: single,
            statPairChars: pair
        }
    }
    // Fixed stat column: as wide as the widest stat in the list, so names end
    // at one x while each stat right-aligns and shows only its non-zero sides.
    readonly property real statWidth: Math.ceil(Math.max(
        root.summary.statSingleChars * statGlyph.advanceWidth,
        root.summary.statPairChars > 0
            ? root.summary.statPairChars * statGlyph.advanceWidth + Theme.spaceSm
            : 0))
    TextMetrics {
        id: statGlyph
        font: Appearance.editorFont(Theme.uiSm, Appearance.fontRevision)
        text: "0"
    }
    TextMetrics { id: countFullMetrics; font: countText.font; text: countText.fullText }
    TextMetrics { id: countNumberMetrics; font: countText.font; text: String(root.fileCount) }

    readonly property int fileCount: summary.entries.length
    readonly property bool anyExpanded: Object.keys(root.expandedPaths).length > 0

    // Flattened rows. Grouped mode adds a header wherever the directory
    // changes; folding a directory hides its file rows without changing the
    // row count, so delegates and scroll stay put.
    readonly property var rows: {
        const entries = root.summary.entries
        const out = []
        let dir = null
        for (let i = 0; i < entries.length; ++i) {
            const entry = entries[i]
            if (root.grouped && entry.dir !== dir) {
                dir = entry.dir
                out.push({ header: true, dir: dir, item: null })
            }
            out.push({ header: false, dir: dir, item: entry.item })
        }
        return out
    }

    // Rows ask for their body while they are expanded and alive; requests
    // made in one frame (a scroll, "expand all") go out as one call. The
    // service answers through diffLinesChanged, relayed as bodiesChanged.
    property var pendingBodies: ({})
    signal bodiesChanged(var paths)

    function requestBody(path) {
        if (!path)
            return
        root.pendingBodies[path] = true
        Qt.callLater(root.flushBodyRequests)
    }
    function flushBodyRequests() {
        const paths = Object.keys(root.pendingBodies)
        root.pendingBodies = ({})
        if (paths.length > 0 && root.gitRepository)
            root.gitRepository.requestDiffLines(root.scope, paths)
    }
    Connections {
        target: root.gitRepository
        ignoreUnknownSignals: true
        function onDiffLinesChanged(scope, paths) {
            if (scope === root.scope)
                root.bodiesChanged(paths)
        }
    }

    // Expanding only marks rows; nothing is fetched until a row is alive.
    function setAllExpanded(expanded) {
        const next = ({})
        if (expanded) {
            const entries = root.summary.entries
            for (let i = 0; i < entries.length; ++i)
                next[entries[i].path] = true
            root.collapsedDirs = ({})
        }
        root.expandedPaths = next
    }

    function togglePath(path) {
        if (!path)
            return
        const next = Object.assign({}, root.expandedPaths)
        if (next[path])
            delete next[path]
        else
            next[path] = true
        root.expandedPaths = next
    }

    function toggleDir(dir) {
        const next = Object.assign({}, root.collapsedDirs)
        if (next[dir])
            delete next[dir]
        else
            next[dir] = true
        root.collapsedDirs = next
    }

    function resetModelState() {
        ++root.modelRevision
        root.expandedPaths = ({})
        root.collapsedDirs = ({})
        root.pendingBodies = ({})
    }
    // Row updates arrive in bursts (a refresh, a batch of bodies syncing
    // their counts); the pass over the model runs once per burst.
    function bumpRevision() { ++root.modelRevision }

    onModelChanged: resetModelState()

    Connections {
        target: root.model
        function onModelReset() { root.resetModelState() }
        function onRowsInserted() { Qt.callLater(root.bumpRevision) }
        function onRowsRemoved() { Qt.callLater(root.bumpRevision) }
        function onDataChanged() { Qt.callLater(root.bumpRevision) }
    }

    ColumnLayout {
        anchors.fill: parent
        spacing: 0
        visible: !root.loading || root.fileCount > 0
        Item {
            Layout.fillWidth: true
            Layout.preferredHeight: Theme.sizeMd

            RowLayout {
                anchors.fill: parent
                anchors.leftMargin: Theme.spaceMd
                anchors.rightMargin: Theme.spaceMd
                spacing: Theme.spaceSm

                // The count takes the leftover width and drops its noun when
                // narrow. The slot owns the layout size, so swapping the text
                // cannot feed back into the width it was chosen from.
                Item {
                    id: countSlot
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    Layout.minimumWidth: Math.ceil(countNumberMetrics.advanceWidth) + 1
                    Layout.preferredWidth: Math.ceil(countFullMetrics.advanceWidth) + 1

                    UiText {
                        id: countText
                        readonly property string fullText: qsTr("%1 files").arg(root.fileCount)
                        anchors.left: parent.left
                        anchors.right: parent.right
                        anchors.verticalCenter: parent.verticalCenter
                        text: countSlot.width >= countFullMetrics.advanceWidth
                              ? fullText : String(root.fileCount)
                        size: "md"
                        elide: Text.ElideRight
                    }
                }
                DiffStat {
                    Layout.alignment: Qt.AlignVCenter
                    Layout.minimumWidth: implicitWidth
                    additions: root.summary.additions
                    deletions: root.summary.deletions
                }
                SegmentedControl {
                    size: "sm"
                    model: [
                        { key: "unified", iconName: "diff-unified", tip: qsTr("Unified diff") },
                        { key: "split", iconName: "diff-split", tip: qsTr("Side-by-side diff") }
                    ]
                    current: root.mode
                    onPicked: key => root.mode = key
                }
                ChromeButton {
                    size: "sm"
                    iconName: "folder"
                    kind: root.grouped ? "selected" : "ghost"
                    tip: root.grouped ? qsTr("Show as list")
                                      : qsTr("Group by directory")
                    enabled: root.fileCount > 0
                    onClicked: root.grouped = !root.grouped
                }
                ChromeButton {
                    size: "sm"
                    iconName: "chevrons-up-down"
                    kind: "ghost"
                    tip: root.anyExpanded ? qsTr("Collapse all diffs")
                                          : qsTr("Expand all diffs")
                    enabled: root.fileCount > 0
                    onClicked: root.setAllExpanded(!root.anyExpanded)
                }
            }
        }

        Item {
            Layout.fillWidth: true
            Layout.fillHeight: true
            visible: root.fileCount > 0

            // Bleeds into the dock gutter so the scrollbar sits on the dock
            // edge, in a lane clear of the expanded bodies' own bars.
            DesktopListView {
                id: fileList
                anchors.fill: parent
                anchors.rightMargin: -Theme.spaceMd
                model: root.rows.length
                clip: true
                boundsBehavior: Flickable.StopAtBounds
                glideHover: true
                reuseItems: true

                // Rows that only fly past during a scrollbar drag stay without
                // a diff body until the view settles.
                property real lastContentY: 0
                onContentYChanged: {
                    if (Math.abs(contentY - lastContentY) > height * 0.5) {
                        root.fastScroll = true
                        settleTimer.restart()
                    } else if (root.fastScroll) {
                        settleTimer.restart()
                    }
                    lastContentY = contentY
                }
                Timer {
                    id: settleTimer
                    interval: Theme.durFast
                    onTriggered: root.fastScroll = false
                }

                delegate: Item {
                    id: section
                    required property int index
                    readonly property var row: root.rows[index] || null
                    readonly property bool isHeader: Boolean(row && row.header)
                    readonly property bool dirOpen: Boolean(row && !root.collapsedDirs[row.dir])
                    readonly property var fileItem: row && !row.header ? row.item : null
                    readonly property bool expanded: Boolean(fileItem
                                                             && root.expandedPaths[fileItem.path])
                    readonly property string filePath: fileItem ? fileItem.path || "" : ""
                    readonly property bool binary: Boolean(fileItem && fileItem.binary)
                    // Latched on once the view is calm: a row created mid-drag
                    // neither converts nor fetches its body until then.
                    property bool live: false
                    readonly property bool wantsBody: expanded && !binary && live

                    readonly property bool shown: isHeader || dirOpen

                    // A copy: the service cache may evict its entry at any time,
                    // and a refetch keeps showing the old body until it lands.
                    property var body: null
                    readonly property bool bodyShown: wantsBody && body !== null
                        && !body.tooLarge && body.error.length === 0

                    function syncBody() {
                        if (!section.wantsBody || !root.gitRepository) {
                            section.body = null
                            return
                        }
                        const fresh = root.gitRepository.diffLines(root.scope, section.filePath)
                        if (fresh)
                            section.body = fresh
                        else
                            root.requestBody(section.filePath)
                    }
                    onWantsBodyChanged: syncBody()
                    onFilePathChanged: {
                        section.body = null
                        section.syncBody()
                    }
                    Component.onCompleted: section.live = !root.fastScroll
                    ListView.onPooled: {
                        section.live = false
                        section.body = null
                    }
                    ListView.onReused: section.live = !root.fastScroll
                    Connections {
                        target: section.live ? null : root
                        function onFastScrollChanged() {
                            if (!root.fastScroll)
                                section.live = true
                        }
                    }
                    Connections {
                        target: section.wantsBody ? root : null
                        function onBodiesChanged(paths) {
                            if (paths.length === 0 || paths.indexOf(section.filePath) >= 0)
                                section.syncBody()
                        }
                    }

                    width: fileList.width - Theme.spaceMd
                    visible: shown
                    height: !shown ? 0 : rowLoader.height + (detail.active ? detail.height : 0)

                    Loader {
                        id: rowLoader
                        width: parent.width
                        sourceComponent: section.isHeader ? dirRowComponent : fileRowComponent
                    }

                    Component {
                        id: dirRowComponent
                        DiffDirRow {
                            path: section.row ? section.row.dir : ""
                            additions: section.group ? section.group.additions : 0
                            deletions: section.group ? section.group.deletions : 0
                            expanded: section.dirOpen
                            statWidth: root.statWidth
                            onToggleRequested: root.toggleDir(section.row ? section.row.dir : "")
                        }
                    }
                    readonly property var group: isHeader ? root.summary.groups[row.dir] || null : null

                    Component {
                        id: fileRowComponent
                        DiffFileRow {
                            indent: root.grouped ? Theme.iconSm + Theme.spaceSm : 0
                            showDirectory: !root.grouped
                            statWidth: root.statWidth
                            path: section.filePath
                            oldPath: section.fileItem ? section.fileItem.oldPath || "" : ""
                            gitStatus: section.fileItem ? section.fileItem.status || "modified"
                                                        : "modified"
                            additions: Number(section.fileItem
                                              ? section.fileItem.additions || 0 : 0)
                            deletions: Number(section.fileItem
                                              ? section.fileItem.deletions || 0 : 0)
                            binary: section.binary
                            expanded: section.expanded
                            onToggleRequested: root.togglePath(section.filePath)
                        }
                    }

                    // Inset like ListItem's surface: flush with the view edge,
                    // the body's scrollbars would stack on the list's.
                    Loader {
                        id: detail
                        anchors.top: rowLoader.bottom
                        x: Theme.spaceSm
                        width: parent.width - Theme.spaceSm * 2
                        active: section.expanded && !section.isHeader
                        sourceComponent: Column {
                            // Not TextLinesView, which would say "no changes".
                            Item {
                                id: diffLoading
                                width: parent.width
                                height: visible ? Theme.sizeMd : 0
                                visible: !section.binary && section.body === null
                                Row {
                                    anchors.centerIn: parent
                                    spacing: Theme.spaceSm
                                    LoadingSpinner {
                                        anchors.verticalCenter: parent.verticalCenter
                                        active: diffLoading.visible && section.live
                                        size: Theme.iconSm
                                    }
                                    UiText {
                                        anchors.verticalCenter: parent.verticalCenter
                                        text: qsTr("Loading diff…")
                                        tone: "muted"
                                        size: "sm"
                                    }
                                }
                            }

                            Loader {
                                width: parent.width
                                active: section.bodyShown
                                sourceComponent: TextLinesView {
                                    underlayColor: Theme.surfaceRaised
                                    height: implicitHeight
                                    model: section.body ? section.body.lines : []
                                    mode: root.mode
                                    openPath: section.filePath
                                    interactiveLines: root.allowWorkspaceOpen && Boolean(root.gitRepository && root.gitRepository.projectPath.length > 0)
                                    maxImplicitHeight: Math.min(360, Math.max(140, fileList.height * 0.55))
                                    onLineActivated: (path, line, col) => {
                                        if (root.allowWorkspaceOpen && root.gitRepository && root.gitRepository.projectPath.length > 0) {
                                            WorkspaceController.openFile(root.gitRepository.projectPath, path, line, col)
                                        }
                                    }
                                }
                            }

                            Item {
                                id: bodyNotice
                                readonly property var body: section.wantsBody ? section.body : null
                                width: parent.width
                                height: visible ? Theme.sizeMd : 0
                                visible: body !== null
                                         && (body.truncated || body.tooLarge || body.error.length > 0)
                                UiText {
                                    anchors.left: parent.left
                                    anchors.right: parent.right
                                    anchors.leftMargin: Theme.spaceMd
                                    anchors.rightMargin: Theme.spaceMd
                                    anchors.verticalCenter: parent.verticalCenter
                                    horizontalAlignment: Text.AlignHCenter
                                    elide: Text.ElideRight
                                    text: {
                                        const body = bodyNotice.body
                                        if (!body)
                                            return ""
                                        if (body.tooLarge)
                                            return qsTr("Diff too large to display")
                                        if (body.error.length > 0)
                                            return body.error
                                        return qsTr("Showing the first %1 of %2 lines")
                                            .arg(body.lines.length).arg(body.totalLines)
                                    }
                                    tone: "muted"
                                    size: "sm"
                                }
                            }

                            Item {
                                width: parent.width
                                height: visible ? Theme.sizeMd : 0
                                visible: section.binary
                                UiText {
                                    anchors.centerIn: parent
                                    text: qsTr("Binary file differs")
                                    tone: "muted"
                                    size: "sm"
                                }
                            }
                        }
                    }
                }
            }
        }

        Item {
            Layout.fillWidth: true
            Layout.fillHeight: true
            visible: root.fileCount === 0 && !root.loading
            UiText {
                anchors.centerIn: parent
                text: root.emptyText
                tone: "muted"
                size: "md"
            }
        }
    }

    PaneLoading {
        anchors.fill: parent
        active: root.loading && root.fileCount === 0
        message: qsTr("Loading changes…")
    }
}
