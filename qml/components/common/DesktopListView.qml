import QtQuick
import QtQuick.Controls
import SpanTui

ListView {
    id: control
    property real wheelLineExtent: Theme.sizeSm
    // Axis-end clearance of the built-in bars; raise it to the corner
    // radius when the view sits flush against a rounded surface.
    property real scrollBarEndInset: Theme.scrollPad

    // Shared gliding hover pill (GlideHighlight): rows carry `glideRow` via
    // ListItem and find the pill by climbing the parent chain — delegates
    // pass contentItem whose parent is this view — for a declared
    // `glideContext` forwarder. Rows' hover/selected chrome yields to it.
    property bool glideHover: false
    readonly property bool glideActive: glide.active
    property var glideContext: glide.glideEnabled ? glide : null
    property alias glideInsetX: glide.insetX
    property alias glideInsetY: glide.insetY
    property alias glideFillColor: glide.fillColor

    acceptedButtons: ScrollPolicy.acceptedButtons
    boundsBehavior: ScrollPolicy.boundsBehavior
    boundsMovement: ScrollPolicy.boundsMovement

    // The pill lives inside contentItem so it shares the delegates'
    // coordinate space and rides its row while content scrolls.
    GlideHighlight {
        id: glide
        parent: control.contentItem
        target: control.contentItem
        glideEnabled: control.glideHover
        // Delegate lookup must skip the view's internal fx items (childAt on
        // contentItem can return them); itemAt() is the canonical delegate
        // hit test, then resolveIn walks wrapper delegates to the glideRow.
        rowAt: pos => {
            const d = control.itemAt(pos.x, pos.y)
            return d ? glide.resolveIn(d, d.mapFromItem(control.contentItem, pos))
                   : null
        }
        // Match the ListItem surface rect (spaceSm horizontal, hairline
        // vertical insets).
        insetX: Theme.spaceSm
        insetY: PixelGrid.hairline(renderDpr)
        fillColor: Theme.hover
        scrollOffset: control.contentY
    }

    // Rebuilt delegates invalidate the tracked row rect.
    onModelChanged: glide.reset()
    // Dynamic delegates (expanded message/diff rows) must resize the pill
    // under a stationary pointer instead of waiting for the next mouse move.
    onContentHeightChanged: glide.refresh()

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
