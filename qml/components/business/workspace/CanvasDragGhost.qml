pragma Translator: "WorkspaceCanvas"

import QtQuick
import SpanTui

// Drag ghost: a floating card centered on the pointer.
Item {
    required property QtObject dragController
    property real devicePixelRatio: 1

    visible: dragController.draggingTileId.length > 0
    // Ghost card width: fixed so the tile label elides instead of the
    // preview ballooning with long titles.
    readonly property int ghostW: 220
    width: ghostW
    height: Theme.sizeMd + Theme.spaceSm * 2
    x: dragController.dragPreviewX - width / 2
    y: dragController.dragPreviewY - height / 2

    CrispSurface {
        anchors.fill: parent
        devicePixelRatio: parent.devicePixelRatio
        radius: Theme.radiusLg
        elevation: "floating"
    }
    UiText {
        anchors.centerIn: parent
        text: {
            const tile = WorkspaceLayout.tileById(parent.dragController.draggingTileId)
            return tile && tile.kind ? tile.kind : qsTr("Pane")
        }
    }
}
