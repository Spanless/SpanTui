import QtQuick
import SpanTui

// One physical-pixel separator. Callers own its length; this component owns
// only thickness so 125%/150% never turns a logical 1px Rectangle into a
// two-sample gray band.
Rectangle {
    id: root

    property bool vertical: false
    property real devicePixelRatio: PixelGrid.safeDpr(Screen.devicePixelRatio)
    property color lineColor: Theme.separator
    property real thicknessPx: 1

    readonly property real thickness: Math.max(1, thicknessPx) / devicePixelRatio

    implicitWidth: vertical ? thickness : 1
    implicitHeight: vertical ? 1 : thickness
    color: lineColor
    antialiasing: false
}
