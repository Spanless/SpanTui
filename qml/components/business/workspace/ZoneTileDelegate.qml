import QtQuick
import SpanTui

// One WorkspaceLayout row rendered as a tile of a canvas zone. Each zone
// repeater walks the whole model and keeps only the rows of its `hostZone`;
// geometry and stacking are assigned by the canvas per zone.
ContentContainer {
    required property string id
    required property string kind
    required property string zone
    required property int row
    required property int rowSpan
    required property int column
    required property string sessionId
    required property string projectPath
    // "left" or "center": the zone this repeater renders.
    required property string hostZone
    required property QtObject dragController

    visible: zone === hostZone
    tileId: id
    tileKind: kind
    tileSessionId: sessionId
    tileProjectPath: projectPath
    tileZone: hostZone
    tileColumn: hostZone === "center" ? column : 0
    tileRow: row
    tileRowSpan: rowSpan
    onDragBegan: globalPos => dragController.beginDrag(id, globalPos)
    onDragMoved: globalPos => dragController.updateDrag(globalPos)
    onDragEnded: globalPos => dragController.endDrag(globalPos)
}
