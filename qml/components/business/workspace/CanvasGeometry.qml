import QtQuick
import SpanTui

// Pure grid geometry for WorkspaceCanvas: column/row placement of center
// tiles and drop hit-testing. Every function reads WorkspaceLayout directly,
// so a binding that calls it re-evaluates when the layout changes.
QtObject {
    id: geometry

    // Owning WorkspaceCanvas (islandX/islandY/islandW/islandH).
    required property Item canvas
    // Center slot size (tiles fill it; the last column stretches to its edge).
    property real slotWidth: 0
    property real slotHeight: 0

    function copyWidths() {
        const src = WorkspaceLayout.centerColumnWidths
        const out = []
        for (let i = 0; i < src.length; ++i)
            out.push(Number(src[i]))
        return out
    }

    function seamXAfterColumn(column, widths) {
        let x = canvas.islandX
        for (let i = 0; i <= column && i < widths.length; ++i)
            x += Number(widths[i])
        return x
    }

    function columnX(column) {
        const widths = WorkspaceLayout.centerColumnWidths
        let x = 0
        for (let i = 0; i < column && i < widths.length; ++i)
            x += Number(widths[i])
        return x
    }

    function columnWidth(column) {
        const widths = WorkspaceLayout.centerColumnWidths
        if (column < 0 || column >= widths.length)
            return 0
        return Number(widths[column])
    }

    // The last column absorbs whatever slot width the stored widths leave.
    function filledColumnWidth(column) {
        const last = WorkspaceLayout.centerColumnWidths.length - 1
        if (column === last)
            return Math.max(columnWidth(column), slotWidth - columnX(column))
        return columnWidth(column)
    }

    function columnRowRatio(column) {
        const ratios = WorkspaceLayout.centerRowRatios
        if (column < 0 || column >= ratios.length)
            return 0.5
        return Number(ratios[column])
    }

    function tileY(row, rowSpan, column) {
        if (rowSpan === 2 || row === 0)
            return 0
        return slotHeight * columnRowRatio(column)
    }

    function tileHeight(row, rowSpan, column) {
        if (rowSpan === 2)
            return slotHeight
        const ratio = columnRowRatio(column)
        if (row === 0)
            return slotHeight * ratio
        return slotHeight * (1 - ratio)
    }

    function dropHit(x, y) {
        const h = Math.max(1, canvas.islandH)
        const widths = WorkspaceLayout.centerColumnWidths
        const localX = x - canvas.islandX
        if (widths.length === 0)
            return ({ column: 0, slot: WorkspaceLayout.dropSlotAt(0.5, (y - canvas.islandY) / h) })
        let acc = 0
        for (let i = 0; i < widths.length; ++i) {
            const w = Number(widths[i])
            if (localX < acc + w) {
                const xInCol = localX - acc
                const xRatio = w > 0 ? xInCol / w : 0.5
                const yRatio = (y - canvas.islandY) / h
                return ({ column: i, slot: WorkspaceLayout.dropSlotAt(xRatio, yRatio) })
            }
            acc += w
        }
        return ({ column: widths.length, slot: 0 })
    }

    function rectFromPreview(preview) {
        const widths = preview.columnWidths
        const col = Number(preview.column)
        let x = canvas.islandX
        for (let i = 0; i < col && i < widths.length; ++i)
            x += Number(widths[i])
        const w = col < widths.length ? Number(widths[col])
                                      : Math.max(440, canvas.islandW)
        const ratio = Number(preview.rowRatio)
        const h = canvas.islandH
        const y0 = canvas.islandY
        if (Number(preview.rowSpan) === 2)
            return Qt.rect(x, y0, w, h)
        if (Number(preview.row) === 0)
            return Qt.rect(x, y0, w, h * ratio)
        return Qt.rect(x, y0 + h * ratio, w, h * (1 - ratio))
    }
}
