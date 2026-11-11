pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import SpanTui

// Workspace canvas: left zone, center tile grid ("island"), right dock and
// the seams between them. Geometry, seam resizing and tile drag-and-drop
// live in CanvasGeometry / CanvasSeamController / TileDragController; this
// file only lays the layers out.
Item {
    id: root

    property bool interactionAnimationsEnabled: true
    readonly property bool resizeActive: seams.seamDragging
    readonly property real renderDpr: PixelGrid.safeDpr(Screen.devicePixelRatio)
    // Flush pane grid: zones and tiles meet on single hairline seams owned by
    // each tile's right/bottom edge — no floating card, no ambient gutters.
    readonly property real hairline: PixelGrid.hairline(renderDpr)
    // Canvas layer order (one sibling stack): zone slots → grid seams →
    // edge handles → perimeter hairlines → seam preview → drop target → ghost.
    readonly property int layerSlot: 1
    readonly property int layerColumnSeam: 20
    readonly property int layerRowSeam: 21
    readonly property int layerEdgeHandle: 30
    readonly property int layerPerimeter: 32
    readonly property int layerDragPreview: 35
    readonly property int layerDropTarget: 40
    readonly property int layerDragGhost: 50
    readonly property real islandX: leftSlot.visible ? WorkspaceLayout.leftWidth : 0
    readonly property real islandY: 0
    readonly property real rightDockWidth: WorkspaceLayout.rightWidth
    readonly property real islandW: Math.max(0, width - islandX - rightDockWidth)
    readonly property real islandH: height

    function previewViewport(): void {
        WindowTransition.previewLocalViewport(Math.round(width), Math.round(height))
    }

    onWidthChanged: previewViewport()
    onHeightChanged: previewViewport()
    Component.onCompleted: previewViewport()

    Connections {
        target: WindowTransition
        function onEpochChanged(): void { root.previewViewport() }
    }

    CanvasGeometry {
        id: geometry
        canvas: root
        slotWidth: centerSlot.width
        slotHeight: centerSlot.height
    }

    CanvasSeamController {
        id: seams
        canvas: root
        geometry: geometry
    }

    TileDragController {
        id: tileDrag
        canvas: root
        geometry: geometry
    }

    Item {
        id: leftSlot
        x: 0
        y: root.islandY
        z: root.layerSlot
        width: WorkspaceLayout.leftWidth
        height: root.islandH
        visible: width > 0

        Repeater {
            model: WorkspaceLayout
            delegate: ZoneTileDelegate {
                hostZone: "left"
                dragController: tileDrag
                canvas: root
                width: leftSlot.width
                height: leftSlot.height
            }
        }
    }

    Item {
        id: island
        objectName: "workspaceCenterCard"
        x: root.islandX
        y: root.islandY
        z: root.layerSlot
        width: root.islandW
        height: root.islandH
        clip: true

        Item {
            id: centerViewport
            anchors.fill: parent
            clip: true

            Item {
                id: centerSlot
                anchors.fill: parent

                Repeater {
                    model: WorkspaceLayout
                    delegate: ZoneTileDelegate {
                        hostZone: "center"
                        dragController: tileDrag
                        canvas: root
                        zoneStacked: rowSpan !== 2
                        width: geometry.filledColumnWidth(column)
                        x: geometry.columnX(column)
                        y: geometry.tileY(row, rowSpan, column)
                        height: geometry.tileHeight(row, rowSpan, column)
                    }
                }

                Repeater {
                    model: Math.max(0, WorkspaceLayout.centerColumnWidths.length - 1)
                    delegate: CanvasSeamHandle {
                        required property int index
                        x: geometry.columnX(index + 1) - width / 2
                        width: Theme.handleW
                        height: centerSlot.height
                        z: root.layerColumnSeam
                        edge: "right"
                        interactionAnimationsEnabled: root.interactionAnimationsEnabled
                        onDragStarted: seams.beginColumnSeam(index)
                        onDragged: delta => seams.updateColumnPreview(delta)
                        onDragEnded: seams.commitColumnSeam()
                    }
                }

                Repeater {
                    model: WorkspaceLayout.centerColumnWidths.length
                    delegate: CanvasSeamHandle {
                        required property int index
                        visible: {
                            const _ = WorkspaceLayout.tilesRevision
                            return WorkspaceLayout.centerColumnStacked(index)
                        }
                        enabled: visible
                        orientation: Qt.Horizontal
                        x: geometry.columnX(index)
                        y: centerSlot.height * geometry.columnRowRatio(index) - height / 2
                        width: geometry.filledColumnWidth(index)
                        height: Theme.handleW
                        z: root.layerRowSeam
                        edge: "bottom"
                        interactionAnimationsEnabled: root.interactionAnimationsEnabled
                        onDragStarted: seams.beginRowSeam(index)
                        onDragged: delta => seams.updateRowPreview(delta)
                        onDragEnded: seams.commitRowSeam()
                    }
                }

                // Grid seam preview (column / row drags), island-local.
                PixelLine {
                    visible: seams.seamDragging && (seams.seamKind === "column"
                                                    || seams.seamKind === "row")
                    z: root.layerDragPreview
                    vertical: seams.seamKind === "column"
                    x: seams.previewLineX - root.islandX
                    y: seams.previewLineY - root.islandY
                    width: vertical ? implicitWidth : seams.previewLineLength
                    height: vertical ? seams.previewLineLength : implicitHeight
                    lineColor: Theme.accent
                }

                CanvasDropTarget {
                    z: root.layerDropTarget
                    canvas: root
                    dragController: tileDrag
                }
            }
        }
    }

    Item {
        id: rightSlot
        x: root.width - width
        y: root.islandY
        z: root.layerSlot
        width: WorkspaceLayout.rightWidth
        height: root.islandH
        visible: width > 0

        RightDock {
            anchors.fill: parent
        }
    }

    CanvasSeamHandle {
        id: leftSeam
        x: root.islandX - width / 2
        y: root.islandY
        width: Theme.handleW
        height: root.islandH
        z: root.layerEdgeHandle
        visible: leftSlot.visible
        railEndInset: 6
        edge: "right"
        interactionAnimationsEnabled: root.interactionAnimationsEnabled
        onDragStarted: seams.beginLeftSeam()
        onDragged: delta => seams.updateLeftPreview(delta)
        onDragEnded: seams.commitLeftSeam()
    }
    CanvasSeamHandle {
        id: rightSeam
        x: root.width - WorkspaceLayout.rightWidth - width / 2
        y: root.islandY
        width: Theme.handleW
        height: root.islandH
        z: root.layerEdgeHandle
        visible: rightSlot.visible
        railEndInset: 6
        edge: "left"
        interactionAnimationsEnabled: root.interactionAnimationsEnabled
        onDragStarted: seams.beginRightSeam()
        onDragged: delta => seams.updateRightPreview(delta)
        onDragEnded: seams.commitRightSeam()
    }
    MouseArea {
        anchors.left: parent.left
        anchors.top: parent.top
        anchors.bottom: parent.bottom
        width: 36  // style-ok: collapsed-sidebar reopen hit strip
        z: root.layerEdgeHandle - 1
        visible: WorkspaceLayout.leftCollapsed
        cursorShape: Qt.PointingHandCursor
        onClicked: WorkspaceLayout.setSideCollapsed("left", false)
    }

    // Edge seam preview (left / right side drags), canvas-local.
    PixelLine {
        visible: seams.seamDragging && (seams.seamKind === "left" || seams.seamKind === "right")
        z: root.layerDragPreview
        vertical: true
        x: seams.previewLineX
        y: seams.previewLineY
        width: implicitWidth
        height: seams.previewLineLength
        lineColor: Theme.accent
    }

    // Window-edge rails: the grid owns the top and left perimeter seams;
    // tiles own the interior plus right/bottom edges through their own
    // right/bottom hairlines.
    PixelLine {
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: parent.top
        z: root.layerPerimeter
        lineColor: Theme.border
    }
    PixelLine {
        anchors.left: parent.left
        anchors.top: parent.top
        anchors.bottom: parent.bottom
        z: root.layerPerimeter
        vertical: true
        lineColor: Theme.border
    }

    CanvasDragGhost {
        z: root.layerDragGhost
        dragController: tileDrag
        devicePixelRatio: root.renderDpr
    }
}
