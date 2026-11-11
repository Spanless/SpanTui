import QtQuick
import SpanTui

// Landing-rect preview for a tile drag, drawn inside the center island.
CrispSurface {
    required property Item canvas
    required property QtObject dragController

    readonly property rect target: dragController.dropRect

    visible: dragController.draggingTileId.length > 0 && target.width > 1
    x: target.x - canvas.islandX
    y: target.y - canvas.islandY
    width: target.width
    height: target.height
    devicePixelRatio: canvas.renderDpr
    radius: 0
    fillEnabled: true
    fillTopColor: Theme.accentSoft
    fillMiddleColor: Theme.accentSoft
    fillBottomColor: Theme.accentSoft
    strokeEnabled: true
    strokeWidthPx: 2
    strokeColor: Theme.accent
    shadowEnabled: false
}
