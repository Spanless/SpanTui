import QtQuick
import SpanTui

// Tile drag-and-drop for WorkspaceCanvas: tracks the dragged tile and the
// pointer in canvas coordinates, previews the landing rect without touching
// the model, and inserts the tile into the center grid on release. Drops over
// the side zones are ignored.
QtObject {
    id: drag

    // Owning WorkspaceCanvas (mapFromGlobal, width).
    required property Item canvas
    required property QtObject geometry

    property string draggingTileId: ""
    property real dragPreviewX: 0
    property real dragPreviewY: 0
    readonly property var dropRect: {
        if (draggingTileId.length === 0)
            return Qt.rect(0, 0, 0, 0)
        return dropTargetRect(draggingTileId, dragPreviewX, dragPreviewY)
    }

    function beginDrag(tileId, globalPos) {
        draggingTileId = tileId
        updateDrag(globalPos)
    }

    function updateDrag(globalPos) {
        const local = canvas.mapFromGlobal(globalPos)
        dragPreviewX = local.x
        dragPreviewY = local.y
    }

    function endDrag(globalPos) {
        const id = draggingTileId
        draggingTileId = ""
        if (id.length === 0)
            return
        const local = canvas.mapFromGlobal(globalPos)
        dropTile(id, local.x, local.y)
    }

    function overCenter(x) {
        return x >= WorkspaceLayout.leftWidth
            && x <= canvas.width - WorkspaceLayout.rightWidth
    }

    function dropTargetRect(id, x, y) {
        if (!overCenter(x))
            return Qt.rect(0, 0, 0, 0)
        const hit = geometry.dropHit(x, y)
        const preview = WorkspaceLayout.previewCenterInsert(id, hit.column, hit.slot)
        if (!preview || !preview.ok)
            return Qt.rect(0, 0, 0, 0)
        return geometry.rectFromPreview(preview)
    }

    function dropTile(id, x, y) {
        if (!overCenter(x))
            return
        const hit = geometry.dropHit(x, y)
        WorkspaceLayout.insertCenterTile(id, hit.column, hit.slot)
    }
}
