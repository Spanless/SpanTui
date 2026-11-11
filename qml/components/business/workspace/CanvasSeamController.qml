import QtQuick
import SpanTui

// Seam-resize state machine for WorkspaceCanvas. A drag previews in memory
// (preview* + one accent preview line) and writes WorkspaceLayout exactly
// once, on release; while a seam is held the transition coordinator runs a
// local resize so tiles freeze their animations.
QtObject {
    id: seams

    // Owning WorkspaceCanvas (width, island geometry).
    required property Item canvas
    required property QtObject geometry

    property bool seamDragging: false
    // "left" | "right" edge seams, "column" | "row" grid seams.
    property string seamKind: ""
    property int seamIndex: 0
    property real previewLeftWidth: 0
    property real previewRightWidth: 0
    property var previewColumnWidths: []
    property real previewRowRatio: 0.5
    property real seamStartValue: 0
    property var seamStartWidths: []
    property real previewLineX: 0
    property real previewLineY: 0
    property real previewLineLength: 0

    function beginSeam(kind: string): void {
        WindowTransition.beginLocalResize()
        seamDragging = true
        seamKind = kind
    }

    function endSeam(): void {
        if (seamDragging)
            WindowTransition.endLocalResize()
        seamDragging = false
        seamKind = ""
    }

    function beginLeftSeam() {
        beginSeam("left")
        seamStartValue = WorkspaceLayout.leftWidth
        updateLeftPreview(0)
    }

    function updateLeftPreview(delta) {
        const raw = Math.round(seamStartValue + delta)
        const otherRight = WorkspaceLayout.rightCollapsed ? 0 : WorkspaceLayout.rightWidth
        const maxLeft = Math.max(250, Math.round(canvas.width - otherRight - 440))
        previewLeftWidth = Math.min(maxLeft, Math.max(120, raw))
        previewLineX = previewLeftWidth
        previewLineY = canvas.islandY + 6
        previewLineLength = Math.max(0, canvas.islandH - 12)
    }

    function commitLeftSeam() {
        if (!seamDragging || seamKind !== "left")
            return
        WorkspaceLayout.setLeftWidth(Math.round(previewLeftWidth), Math.round(canvas.width))
        endSeam()
    }

    function beginRightSeam() {
        beginSeam("right")
        seamStartValue = WorkspaceLayout.rightWidth
        updateRightPreview(0)
    }

    function updateRightPreview(delta) {
        const raw = Math.round(seamStartValue - delta)
        const otherLeft = WorkspaceLayout.leftCollapsed ? 0 : WorkspaceLayout.leftWidth
        const maxRight = Math.max(240, Math.round(canvas.width - otherLeft - 440))
        previewRightWidth = Math.min(maxRight, Math.max(120, raw))
        previewLineX = canvas.width - previewRightWidth
        previewLineY = canvas.islandY + 6
        previewLineLength = Math.max(0, canvas.islandH - 12)
    }

    function commitRightSeam() {
        if (!seamDragging || seamKind !== "right")
            return
        WorkspaceLayout.setRightWidth(Math.round(previewRightWidth), Math.round(canvas.width))
        endSeam()
    }

    function beginColumnSeam(index) {
        beginSeam("column")
        seamIndex = index
        seamStartWidths = geometry.copyWidths()
        seamStartValue = Number(seamStartWidths[index] || 0)
        updateColumnPreview(0)
    }

    function updateColumnPreview(delta) {
        const desired = Math.round(seamStartValue + delta)
        const clamped = WorkspaceLayout.clampedCenterColumnWidth(seamIndex, desired)
        const widths = []
        for (let i = 0; i < seamStartWidths.length; ++i)
            widths.push(Number(seamStartWidths[i]))
        if (seamIndex + 1 < widths.length) {
            const pair = Number(seamStartWidths[seamIndex])
                        + Number(seamStartWidths[seamIndex + 1])
            widths[seamIndex] = clamped
            widths[seamIndex + 1] = pair - clamped
        }
        previewColumnWidths = widths
        previewLineX = geometry.seamXAfterColumn(seamIndex, widths)
        previewLineY = canvas.islandY
        previewLineLength = canvas.islandH
    }

    function commitColumnSeam() {
        if (!seamDragging || seamKind !== "column")
            return
        WorkspaceLayout.setCenterColumnWidth(
                    seamIndex, Math.round(Number(previewColumnWidths[seamIndex])))
        endSeam()
    }

    function beginRowSeam(column) {
        beginSeam("row")
        seamIndex = column
        seamStartValue = geometry.columnRowRatio(column)
        updateRowPreview(0)
    }

    function updateRowPreview(delta) {
        const h = Math.max(1, canvas.islandH)
        previewRowRatio = WorkspaceLayout.clampedCenterRowRatio(seamStartValue + delta / h)
        previewLineX = canvas.islandX + geometry.columnX(seamIndex)
        previewLineY = canvas.islandY + h * previewRowRatio
        previewLineLength = geometry.filledColumnWidth(seamIndex)
    }

    function commitRowSeam() {
        if (!seamDragging || seamKind !== "row")
            return
        WorkspaceLayout.setCenterRowRatio(seamIndex, previewRowRatio)
        endSeam()
    }
}
