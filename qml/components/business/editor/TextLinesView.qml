pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import SpanTui

// Shared read-only line list for file preview and git diffs.
// In `file` mode `model` is a CodePreviewLineModel (roles text, gitStatus,
// deletionBefore; properties count, maxColumns). In `unified` / `split` mode
// it is a JS array of rows { kind, oldLine, newLine, oldText, newText }.
Item {
    id: root

    property var model: null
    property string mode: "unified"
    property var highlightLines: ({})
    property string emptyText: qsTr("No textual changes")
    property string openPath: ""
    property bool interactiveLines: false
    property int emptyHeight: 48
    property int maxImplicitHeight: 280
    property int modelRevision: 0
    property var splitRows: []
    // The plane behind the lines: document paper by default; panel hosts
    // (inline commit diffs) pass surfaceRaised. Diff tints and row states are
    // derived against it.
    property color underlayColor: Theme.surfaceBase

    readonly property real renderDpr: PixelGrid.safeDpr(Screen.devicePixelRatio)
    readonly property real hairline: PixelGrid.hairline(renderDpr)
    readonly property int gutterWidth: 42
    readonly property int rowCount: !root.model ? 0
                                    : root.mode === "file" ? root.model.count
                                    : root.model.length
    readonly property int visualRowCount: mode === "split"
                                                  ? splitRows.length : rowCount
    readonly property real contentCanvasWidth: {
        const revision = root.modelRevision
        if (root.mode === "split")
            return root.calculateSplitContentWidth()
        if (root.mode === "file")
            return root.calculateFileContentWidth()
        return root.calculateUnifiedContentWidth()
    }

    signal lineActivated(string path, int line, int column)

    // Row height follows the configured editor font size and line-height
    // multiplier instead of a fixed 23px value that clipped larger fonts.
    readonly property real lineHeight: Math.max(
        18, Math.ceil(diffFontMetrics.height
                      * Math.max(1.0, Appearance.editorLineHeight)))
    implicitHeight: Math.min(maxImplicitHeight,
                             visualRowCount > 0
                             ? visualRowCount * root.lineHeight : emptyHeight)
    clip: true
    focus: true

    function rowColor(kind: string): color {
        return Theme.diffTint(root.underlayColor, kind)
    }

    function gitTrackColor(item: var): color {
        if (!item)
            return "transparent"
        if (item.gitStatus === "added" || item.kind === "add")
            return Theme.gitAdded
        if (item.gitStatus === "modified" || item.kind === "change")
            return Theme.gitModified
        if (item.deletionBefore)
            return Theme.gitDeleted
        return "transparent"
    }

    function gutterColor(kind: string, side: string): color {
        if (side === "old" && kind === "delete")
            return Theme.gitDeleted
        if (side === "new" && kind === "add")
            return Theme.gitAdded
        return Theme.muted
    }

    function unifiedLineText(item: var): string {
        if (!item)
            return ""
        if (item.kind === "delete")
            return item.oldText || ""
        return item.newText || ""
    }

    function modelItem(index: int): var {
        return root.model ? root.model[index] : null
    }

    readonly property real charWidth: Math.max(1, diffFontMetrics.advanceWidth("M") || diffFontMetrics.averageCharacterWidth)

    function expandedLength(value: var): int {
        if (!value)
            return 0
        const text = String(value)
        let columns = 0
        for (let i = 0; i < text.length; ++i) {
            if (text.charCodeAt(i) === 9) {
                columns += (4 - columns % 4)
            } else {
                ++columns
            }
        }
        return columns
    }

    function calculateFileContentWidth(): real {
        const maxCols = root.model ? root.model.maxColumns : 0
        const maxTextWidth = Math.ceil(maxCols * root.charWidth)
        const trailing = Theme.spaceMd * 2 + Theme.spaceMd
        return Math.max(diffList.width,
                        Math.ceil(4 + root.gutterWidth + trailing + maxTextWidth))
    }

    function calculateUnifiedContentWidth(): real {
        let maxCols = 0
        const count = root.rowCount
        for (let index = 0; index < count; ++index) {
            const text = root.unifiedLineText(root.modelItem(index))
            if (text.length > 0)
                maxCols = Math.max(maxCols, root.expandedLength(text))
        }
        const maxTextWidth = Math.ceil(maxCols * root.charWidth)
        const trailing = Theme.spaceMd * 2 + Theme.spaceMd * 2
        return Math.max(diffList.width,
                        Math.ceil(root.gutterWidth * 2 + trailing + maxTextWidth))
    }

    function calculateSplitContentWidth(): real {
        let maxCols = 0
        const rows = root.splitRows
        for (let index = 0; index < rows.length; ++index) {
            const item = rows[index]
            if (item) {
                if (item.oldText && item.oldText.length > 0)
                    maxCols = Math.max(maxCols, root.expandedLength(item.oldText))
                if (item.newText && item.newText.length > 0)
                    maxCols = Math.max(maxCols, root.expandedLength(item.newText))
            }
        }
        const maxTextWidth = Math.ceil(maxCols * root.charWidth)
        const trailing = Theme.spaceMd + Theme.spaceMd
        const paneWidth = Math.max(diffList.width / 2,
                                   root.gutterWidth + trailing + maxTextWidth)
        return Math.max(diffList.width, Math.ceil(paneWidth * 2 + root.hairline))
    }

    function rebuildSplitRows(): void {
        if (root.mode !== "split") {
            root.splitRows = []
            return
        }
        const rows = []
        let index = 0
        while (index < root.rowCount) {
            const item = root.modelItem(index) || ({})
            if (item.kind !== "delete") {
                rows.push({
                    kind: item.kind,
                    oldLine: item.oldLine || 0,
                    newLine: item.newLine || 0,
                    oldText: item.oldText || "",
                    newText: item.newText || "",
                    oldChanged: false,
                    newChanged: item.kind === "add"
                })
                ++index
                continue
            }

            const deleted = []
            const added = []
            while (index < root.rowCount) {
                const candidate = root.modelItem(index) || ({})
                if (candidate.kind !== "delete")
                    break
                deleted.push(candidate)
                ++index
            }
            while (index < root.rowCount) {
                const candidate = root.modelItem(index) || ({})
                if (candidate.kind !== "add")
                    break
                added.push(candidate)
                ++index
            }
            const pairedCount = Math.max(deleted.length, added.length)
            for (let pair = 0; pair < pairedCount; ++pair) {
                const oldItem = pair < deleted.length ? deleted[pair] : ({})
                const newItem = pair < added.length ? added[pair] : ({})
                rows.push({
                    kind: "change",
                    oldLine: oldItem.oldLine || 0,
                    newLine: newItem.newLine || 0,
                    oldText: oldItem.oldText || "",
                    newText: newItem.newText || "",
                    oldChanged: pair < deleted.length,
                    newChanged: pair < added.length
                })
            }
        }
        root.splitRows = rows
    }

    function setCurrentLine(oneBasedLine: int, center: bool): void {
        const index = Math.max(0, Number(oneBasedLine || 1) - 1)
        diffList.positionViewAtIndex(index, center ? ListView.Center : ListView.Contain)
        currentLine = oneBasedLine
    }

    function positionAtLine(oneBasedLine: int): void {
        setCurrentLine(oneBasedLine, true)
    }

    property int currentLine: 1

    function resetViewport(): void {
        ++modelRevision
        rebuildSplitRows()
        Qt.callLater(function() {
            diffList.contentX = 0
            if (root.mode !== "file")
                diffList.contentY = 0
        })
    }

    onModelChanged: resetViewport()
    // The file-mode model object stays the same across documents; a reset
    // means a new text.
    Connections {
        target: root.mode === "file" ? root.model : null
        function onModelReset(): void { root.resetViewport() }
    }
    onModeChanged: {
        ++modelRevision
        rebuildSplitRows()
        diffList.contentX = 0
    }

    FontMetrics {
        id: diffFontMetrics
        font: Appearance.editorFont(Theme.editorFontSize, Appearance.fontRevision)
    }

    DesktopListView {
        id: diffList
        // Fills the pane so the overlay scrollbars sit on its edges; the
        // canvas width already carries the trailing text gutter.
        anchors.fill: parent
        // Deliberately no GlideHighlight: each line surface already encodes
        // diff kind, search hits and current-line state. A shared hover pill
        // would either be hidden by or erase those semantic backgrounds.
        model: root.mode === "split" ? root.splitRows : root.model
        clip: true
        reuseItems: root.mode === "file"
        boundsBehavior: Flickable.StopAtBounds
        flickableDirection: Flickable.HorizontalAndVerticalFlick
        contentWidth: root.contentCanvasWidth

        delegate: Rectangle {
            id: diffRow
            required property int index
            // File mode reads the line model's roles through `model`; diff
            // modes index the JS rows (inline commit sections are QVariantLists).
            required property var model
            readonly property var lineItem: root.mode === "split"
                ? root.splitRows[index] || ({})
                : root.mode === "file"
                  ? ({ newLine: index + 1, newText: diffRow.model.text,
                       gitStatus: diffRow.model.gitStatus,
                       deletionBefore: diffRow.model.deletionBefore })
                  : root.modelItem(index) || ({})

            width: diffList.contentWidth
            height: root.lineHeight
            // Resting tone (diff tint, current line or the bare plane);
            // pointer states stack on it instead of replacing it.
            readonly property color restColor: root.mode === "file"
                ? (root.currentLine === diffRow.lineItem.newLine
                   ? Theme.editorCurrentLine : root.underlayColor)
                : root.rowColor(diffRow.lineItem.kind)
            color: {
                if (root.mode === "file" && root.highlightLines[diffRow.lineItem.newLine])
                    return Theme.editorSearchMatch
                if (rowMouse.enabled && rowMouse.pressed)
                    return Theme.stateFill(diffRow.restColor, "pressed")
                if (rowMouse.enabled && rowMouse.containsMouse)
                    return Theme.stateFill(diffRow.restColor, "hover")
                return diffRow.restColor
            }

            RowLayout {
                anchors.fill: parent
                spacing: 0
                visible: root.mode === "file"

                Rectangle {
                    Layout.preferredWidth: 4 // style-ok: git hunk track stripe
                    Layout.fillHeight: true
                    color: root.gitTrackColor(diffRow.lineItem)
                }
                MonoText {
                    Layout.preferredWidth: root.gutterWidth
                    horizontalAlignment: Text.AlignRight
                    rightPadding: Theme.spaceMd
                    text: diffRow.lineItem.newLine || ""
                    color: Theme.editorGutterText
                }
                MonoText {
                    Layout.fillWidth: true
                    leftPadding: Theme.spaceMd
                    text: diffRow.lineItem.newText || ""
                    color: Theme.text
                    wrapMode: Text.NoWrap
                    elide: Text.ElideNone
                }
            }

            RowLayout {
                id: unifiedContent
                anchors.fill: parent
                spacing: 0
                visible: root.mode === "unified"

                MonoText {
                    Layout.preferredWidth: root.gutterWidth
                    horizontalAlignment: Text.AlignRight
                    rightPadding: Theme.spaceMd
                    text: diffRow.lineItem.oldLine || ""
                    color: root.gutterColor(diffRow.lineItem.kind, "old")
                }
                MonoText {
                    Layout.preferredWidth: root.gutterWidth
                    horizontalAlignment: Text.AlignRight
                    rightPadding: Theme.spaceMd
                    text: diffRow.lineItem.newLine || ""
                    color: root.gutterColor(diffRow.lineItem.kind, "new")
                }
                MonoText {
                    Layout.fillWidth: true
                    leftPadding: Theme.spaceMd
                    text: root.unifiedLineText(diffRow.lineItem)
                    color: diffRow.lineItem.kind === "add" ? Theme.gitAdded
                        : diffRow.lineItem.kind === "delete" ? Theme.gitDeleted
                        : Theme.text
                    wrapMode: Text.NoWrap
                    elide: Text.ElideNone
                }
            }

            RowLayout {
                anchors.fill: parent
                spacing: root.hairline
                visible: root.mode === "split"

                Rectangle {
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    color: diffRow.lineItem.oldChanged
                           ? Theme.diffTint(root.underlayColor, "delete") : "transparent"
                    RowLayout {
                        anchors.fill: parent
                        spacing: 0
                        MonoText {
                            Layout.preferredWidth: root.gutterWidth
                            horizontalAlignment: Text.AlignRight
                            rightPadding: Theme.spaceMd
                            text: diffRow.lineItem.oldLine || ""
                            color: diffRow.lineItem.oldChanged ? Theme.gitDeleted : Theme.muted
                        }
                        MonoText {
                            Layout.fillWidth: true
                            text: diffRow.lineItem.oldText || ""
                            color: diffRow.lineItem.oldChanged ? Theme.gitDeleted : Theme.text
                            wrapMode: Text.NoWrap
                            elide: Text.ElideNone
                        }
                    }
                }
                Rectangle {
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    color: diffRow.lineItem.newChanged
                           ? Theme.diffTint(root.underlayColor, "add") : "transparent"
                    RowLayout {
                        anchors.fill: parent
                        spacing: 0
                        MonoText {
                            Layout.preferredWidth: root.gutterWidth
                            horizontalAlignment: Text.AlignRight
                            rightPadding: Theme.spaceMd
                            text: diffRow.lineItem.newLine || ""
                            color: diffRow.lineItem.newChanged ? Theme.gitAdded : Theme.muted
                        }
                        MonoText {
                            Layout.fillWidth: true
                            text: diffRow.lineItem.newText || ""
                            color: diffRow.lineItem.newChanged ? Theme.gitAdded : Theme.text
                            wrapMode: Text.NoWrap
                            elide: Text.ElideNone
                        }
                    }
                }
            }

            MouseArea {
                id: rowMouse
                anchors.fill: parent
                enabled: (root.interactiveLines && root.openPath.length > 0) || root.mode === "file"
                hoverEnabled: true
                cursorShape: (root.interactiveLines && root.openPath.length > 0) ? Qt.PointingHandCursor : Qt.ArrowCursor
                onClicked: {
                    root.forceActiveFocus()
                    if (root.mode === "file") {
                        root.currentLine = diffRow.lineItem.newLine || 1
                    }
                    if (root.interactiveLines && root.openPath.length > 0) {
                        const line = diffRow.lineItem.newLine > 0
                                   ? diffRow.lineItem.newLine : diffRow.lineItem.oldLine
                        root.lineActivated(root.openPath, line || 1, 1)
                    }
                }
            }
        }

        footer: Item {
            width: diffList.width
            height: root.visualRowCount === 0 ? root.emptyHeight : 0
            UiText {
                anchors.centerIn: parent
                text: root.emptyText
                tone: "muted"
                visible: parent.height > 0
            }
        }
    }
}
