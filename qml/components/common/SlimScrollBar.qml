import QtQuick
import QtQuick.Controls
import SpanTui

// The one overlay scrollbar, for both axes. Hidden at rest; revealed while
// its viewport scrolls (lingering a beat after the last movement) or while
// the pointer is near its edge. Hovering the strip or dragging widens the
// thumb and lays a faint track under it. Neutral in every state — contrast
// comes from the fade ladder, never from the accent.
//
// One scrollEdgeGap off the viewport edge: the bar lives in the lane every
// row already leaves empty (ListItem's spaceSm surface inset plus its
// spaceMd content padding), so it never covers content and never takes
// layout width.
//
// Scroll containers (DesktopScrollView, DesktopListView, DesktopFlickable)
// build a vertical and a horizontal bar in; business code does not declare
// bars. A custom viewport (terminal scrollback) drives `size`/`position`
// itself and calls pulse() when it scrolls.
ScrollBar {
    id: control
    readonly property real renderDpr: PixelGrid.safeDpr(Screen.devicePixelRatio)
    readonly property real axisExtent: vertical ? height : width

    // The Flickable whose contentX/contentY movement pulses this bar. An
    // attached bar defaults to its parent view; a ScrollView passes its
    // internal Flickable.
    property Item viewport: ScrollPolicy.isScrollViewport(parent) ? parent : null
    // Clearance at both axis ends; hosts flush against a rounded surface
    // raise it to the corner radius so the thumb stays inside the arc.
    property real endInset: Theme.scrollPad
    // Trailing end kept clear of the thumb — the corner where the other
    // axis' bar runs.
    property real endReserve: 0
    // Center the thumb across the bar instead of hugging the outer edge.
    // For a bar that fills a dedicated gutter beside its viewport.
    property bool centered: false

    // Thumb thickness at rest (the row surface inset) and while
    // hovered/dragged; the hit strip is the row content padding.
    readonly property real restThickness: PixelGrid.snapSize(Theme.spaceSm, renderDpr)
    readonly property real hoverThickness: PixelGrid.snapSize(6, renderDpr)  // style-ok: stays inside the spaceMd content padding
    readonly property real stripThickness: PixelGrid.snapSize(Theme.spaceMd, renderDpr)
    readonly property real edgeGap: PixelGrid.snapSize(Theme.scrollEdgeGap, renderDpr)
    // Pointer distance beyond the strip that still reveals the bar.
    readonly property real nearDistance: Theme.spaceLg

    property bool _scrolling: false
    readonly property bool expanded: hovered || pressed
    readonly property bool near: {
        if (!nearSense.hovered)
            return false
        const p = nearSense.point.position
        const along = vertical ? p.y : p.x
        const start = vertical ? y : x
        if (along < start || along > start + axisExtent)
            return false
        const across = vertical ? p.x : p.y
        const low = vertical ? x : y
        const high = low + (vertical ? width : height)
        const gap = across < low ? low - across : (across > high ? across - high : 0)
        return gap <= nearDistance
    }
    readonly property bool revealed: policy === ScrollBar.AlwaysOn
                                     || _scrolling || expanded || near

    function pulse() {
        _scrolling = true
        lingerTimer.restart()
    }

    policy: ScrollBar.AsNeeded
    // Only an overflowing axis exists: a hidden bar must not keep eating
    // clicks along the view edge.
    visible: policy !== ScrollBar.AlwaysOff && size < 1
    // Keep the visual handle usable without corrupting the logical `size`.
    // ScrollBar maps visualSize/visualPosition back to the real range.
    minimumSize: axisExtent > 0
                 ? Math.min(1, PixelGrid.snapSize(Theme.sizeMd, renderDpr) / axisExtent)
                 : 0
    implicitWidth: stripThickness
    implicitHeight: stripThickness
    padding: 0
    topPadding: vertical ? endInset : 0
    bottomPadding: vertical ? endInset + endReserve : 0
    leftPadding: vertical ? 0 : endInset
    rightPadding: vertical ? 0 : endInset + endReserve

    hoverEnabled: true
    wheelEnabled: false

    // A thumb drag is a new absolute-position interaction. Any kinetic motion
    // still owned by the viewport must stop before the thumb moves.
    onPressedChanged: {
        if (pressed && viewport && typeof viewport.cancelFlick === "function")
            viewport.cancelFlick()
    }

    Timer {
        id: lingerTimer
        interval: Theme.durSlow * 2
        onTriggered: control._scrolling = false
    }
    Connections {
        target: control.viewport
        ignoreUnknownSignals: true
        function onContentXChanged() { if (!control.vertical) control.pulse() }
        function onContentYChanged() { if (control.vertical) control.pulse() }
    }

    // Proximity is geometric on the host item, so the bar reveals before the
    // pointer reaches its strip and regardless of which child is hovered.
    HoverHandler {
        id: nearSense
        parent: control.parent ? control.parent : control
        enabled: control.visible
        acceptedDevices: PointerDevice.Mouse | PointerDevice.TouchPad
    }

    // Thickness shared by track and thumb, anchored edgeGap inside the outer
    // edge; the bar grows inward.
    property real _thickness: expanded ? hoverThickness : restThickness
    Behavior on _thickness {
        enabled: !Appearance.reduceMotion
        NumberAnimation { duration: Theme.durFast; easing.type: Easing.OutCubic }
    }
    readonly property bool leadingEdge: vertical && mirrored
    function _acrossOffset(extent) {
        if (centered)
            return PixelGrid.snapSize((extent - _thickness) / 2, renderDpr)
        return leadingEdge ? edgeGap : extent - edgeGap - _thickness
    }

    background: Item {
        implicitWidth: control.stripThickness
        implicitHeight: control.stripThickness

        CrispSurface {
            x: control.vertical ? control._acrossOffset(parent.width) : control.leftPadding
            y: control.vertical ? control.topPadding : control._acrossOffset(parent.height)
            width: control.vertical ? control._thickness : control.availableWidth
            height: control.vertical ? control.availableHeight : control._thickness
            devicePixelRatio: control.renderDpr
            radius: control._thickness / 2
            fillTopColor: Theme.stateHover
            fillMiddleColor: fillTopColor
            fillBottomColor: fillTopColor
            strokeEnabled: false
            opacity: control.expanded && control.revealed ? 1 : 0

            Behavior on opacity {
                enabled: !Appearance.reduceMotion
                NumberAnimation { duration: Theme.durFast; easing.type: Easing.OutCubic }
            }
        }
    }

    contentItem: Item {
        implicitWidth: control.stripThickness
        implicitHeight: control.stripThickness

        CrispSurface {
            x: control.vertical ? control._acrossOffset(parent.width) : 0
            y: control.vertical ? 0 : control._acrossOffset(parent.height)
            width: control.vertical ? control._thickness : parent.width
            height: control.vertical ? parent.height : control._thickness
            devicePixelRatio: control.renderDpr
            radius: control._thickness / 2
            fillTopColor: Theme.muted
            fillMiddleColor: fillTopColor
            fillBottomColor: fillTopColor
            strokeEnabled: false
            // Rest → hover → drag climbs the fade ladder on one mid gray.
            opacity: !control.revealed ? 0
                     : control.pressed ? 1
                     : control.hovered ? Theme.fadeLg
                     : Theme.fadeMd

            Behavior on opacity {
                enabled: !Appearance.reduceMotion
                NumberAnimation { duration: Theme.durMid; easing.type: Easing.OutCubic }
            }
        }
    }
}
