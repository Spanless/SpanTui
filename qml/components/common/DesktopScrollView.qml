import QtQuick
import QtQuick.Controls
import SpanTui

// The one desktop scroll container. Content sits inside a 2px edge inset
// (Theme.scrollPad); the two SlimScrollBar overlays (right vertical, bottom
// horizontal) never reserve layout space and auto-hide on their own. Wheel
// policy lives in ScrollPolicy: vertical wheel travel moves only the
// vertical axis; the horizontal axis takes horizontal input only.
ScrollView {
    id: control
    property real wheelLineExtent: Theme.sizeSm
    // Axis-end clearance of the built-in bars; raise it to the corner
    // radius when the view sits flush against a rounded surface.
    property real scrollBarEndInset: Theme.scrollPad

    // DesktopWheelHandler is the sole wheel owner for the internal Flickable.
    wheelEnabled: false

    // Uniform content inset; the overlay bars sit on the outer edges.
    padding: Theme.scrollPad

    // ScrollView owns its internal Flickable, so bind the shared policy at
    // that ownership boundary instead of relying on Qt's touch-first defaults.
    Binding {
        target: control.contentItem
        property: "acceptedButtons"
        value: ScrollPolicy.acceptedButtons
        when: control.contentItem !== null
    }
    Binding {
        target: control.contentItem
        property: "boundsBehavior"
        value: ScrollPolicy.boundsBehavior
        when: control.contentItem !== null
    }
    Binding {
        target: control.contentItem
        property: "boundsMovement"
        value: ScrollPolicy.boundsMovement
        when: control.contentItem !== null
    }

    DesktopWheelHandler {
        parent: control
        viewport: control.contentItem
        lineExtent: control.wheelLineExtent
        wheelScrollLines: Appearance.wheelScrollLines
    }

    // Laid out like the Basic style's bars (parented to the view, flush to
    // its outer edges); each keeps the shared corner clear of its thumb.
    ScrollBar.vertical: SlimScrollBar {
        id: vBar
        parent: control
        viewport: control.contentItem
        x: control.mirrored ? 0 : control.width - width
        height: control.height
        endInset: control.scrollBarEndInset
        endReserve: hBar.visible ? hBar.height : 0
    }
    ScrollBar.horizontal: SlimScrollBar {
        id: hBar
        parent: control
        viewport: control.contentItem
        y: control.height - height
        width: control.width
        endInset: control.scrollBarEndInset
        endReserve: vBar.visible ? vBar.width : 0
    }
}
