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
    // A floating side (window too narrow to dock it) slides in over the
    // canvas, above everything the canvas draws; the title bar stays above.
    readonly property int layerOverlayCatcher: 60
    readonly property int layerOverlay: 61
    // The canvas share a floating side may cover.
    readonly property real overlayMaxShare: 0.85
    readonly property bool overlayOpen: WorkspaceLayout.leftOverlayOpen || WorkspaceLayout.rightOverlayOpen
    readonly property real islandX: WorkspaceLayout.leftWidth
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

    function overlayWidth(panelWidth: real): real {
        return Math.min(panelWidth, Math.round(root.width * root.overlayMaxShare))
    }

    // Focus moves into an opened overlay (so Esc reaches it) and back to
    // where it was when the overlay closes.
    readonly property string openOverlay: WorkspaceLayout.leftOverlayOpen ? "left"
                                          : WorkspaceLayout.rightOverlayOpen ? "right" : ""
    property Item focusBeforeOverlay: null
    onOpenOverlayChanged: {
        if (openOverlay.length > 0) {
            if (!focusBeforeOverlay)
                focusBeforeOverlay = Window.activeFocusItem
            const slot = openOverlay === "left" ? leftSlot : rightSlot
            slot.forceActiveFocus()
        } else if (focusBeforeOverlay) {
            focusBeforeOverlay.forceActiveFocus()
            focusBeforeOverlay = null
        }
    }

    // Under an open overlay: a press anywhere else on the canvas closes it
    // and goes no further. The title bar is above the canvas, so its side
    // toggles keep working as toggles.
    MouseArea {
        anchors.fill: parent
        z: root.layerOverlayCatcher
        visible: root.overlayOpen
        acceptedButtons: Qt.AllButtons
        onPressed: WorkspaceLayout.closeOverlays()
        onWheel: wheel => { wheel.accepted = true }
    }

    CrispSurface {
        z: root.layerOverlay - 1
        x: leftSlot.x
        y: leftSlot.y
        width: leftSlot.width
        height: leftSlot.height
        visible: leftSlot.floating && leftSlot.visible
        elevation: "floating"
        fillTopColor: Theme.surfaceBase
        strokeEnabled: false
    }
    CrispSurface {
        z: root.layerOverlay - 1
        x: rightSlot.x
        y: rightSlot.y
        width: rightSlot.width
        height: rightSlot.height
        visible: rightSlot.floating && rightSlot.visible
        elevation: "floating"
        fillTopColor: Theme.surfaceBase
        strokeEnabled: false
    }

    // Docked, a side is a column of the canvas; floating, the same item
    // (and the same tiles in it) slides in from its edge as an overlay.
    FocusScope {
        id: leftSlot
        readonly property bool floating: WorkspaceLayout.leftFloating
        readonly property bool open: WorkspaceLayout.leftOverlayOpen
        x: floating && !open ? -width : 0
        y: root.islandY
        z: floating ? root.layerOverlay : root.layerSlot
        width: floating ? root.overlayWidth(WorkspaceLayout.leftPanelWidth) : WorkspaceLayout.leftWidth
        height: root.islandH
        visible: floating ? open || slideIn.running : width > 0
        Keys.onEscapePressed: WorkspaceLayout.closeOverlays()

        Behavior on x {
            enabled: leftSlot.floating && root.interactionAnimationsEnabled
            NumberAnimation { id: slideIn; duration: Theme.durMid; easing.type: Easing.OutCubic }
        }

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

    FocusScope {
        id: rightSlot
        readonly property bool floating: WorkspaceLayout.rightFloating
        readonly property bool open: WorkspaceLayout.rightOverlayOpen
        x: floating && !open ? root.width : root.width - width
        y: root.islandY
        z: floating ? root.layerOverlay : root.layerSlot
        width: floating ? root.overlayWidth(WorkspaceLayout.rightPanelWidth) : WorkspaceLayout.rightWidth
        height: root.islandH
        visible: floating ? open || slideInRight.running : width > 0
        Keys.onEscapePressed: WorkspaceLayout.closeOverlays()

        Behavior on x {
            enabled: rightSlot.floating && root.interactionAnimationsEnabled
            NumberAnimation { id: slideInRight; duration: Theme.durMid; easing.type: Easing.OutCubic }
        }

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
        visible: WorkspaceLayout.leftWidth > 0
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
        visible: WorkspaceLayout.rightWidth > 0
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
        onClicked: WorkspaceController.toggleSidePanel("left")
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

    // Window-edge rail: the grid owns the left perimeter seam; tiles and the
    // right dock own interior plus right/bottom seams, and each zone's top
    // header owns the aligned horizontal hairline under the shared top band.
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
