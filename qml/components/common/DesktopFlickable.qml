import QtQuick
import QtQuick.Controls
import SpanTui

Flickable {
    id: control
    property real wheelLineExtent: Theme.sizeSm
    // Axis-end clearance of the built-in bars; raise it to the corner
    // radius when the view sits flush against a rounded surface.
    property real scrollBarEndInset: Theme.scrollPad

    acceptedButtons: ScrollPolicy.acceptedButtons
    boundsBehavior: ScrollPolicy.boundsBehavior
    boundsMovement: ScrollPolicy.boundsMovement

    DesktopWheelHandler {
        parent: control
        viewport: control
        lineExtent: control.wheelLineExtent
        wheelScrollLines: Appearance.wheelScrollLines
    }

    // Both axes built in; each shows only while its axis overflows.
    ScrollBar.vertical: SlimScrollBar {
        id: vBar
        endInset: control.scrollBarEndInset
        endReserve: hBar.visible ? hBar.height : 0
    }
    ScrollBar.horizontal: SlimScrollBar {
        id: hBar
        endInset: control.scrollBarEndInset
        endReserve: vBar.visible ? vBar.width : 0
    }
}
