import QtQuick
import SpanTui

// One hover pill that glides between rows instead of each row painting its
// own hover. `target` is the row container: hit tests and the pill frame
// map through it.
//
// Rows: `glideRow === true` is a target (ListItem), `false` a hard skip that
// keeps the last row, absent a wrapper the hit test descends into;
// `glideDanger === true` uses dangerFillColor.
//
// Rows find the pill by climbing parents for `glideContext`. QML has no
// dynamic properties, so a host ancestor declares
// `property var glideContext: glide.glideEnabled ? glide : null`; the wrap
// form (GlideHighlight { Column {…} }) is covered by the pill's own property.
//
// Hosts that recolor the hovered label compare against `currentRow`, not the
// row's hover: the pill keeps its last row while the pointer crosses a gap.
Item {
    id: root

    property Item target: parent
    // (pos in target coords) → row Item or null.
    property var rowAt: null
    // Not Item.enabled: in the wrap form that would also disable the rows.
    property bool glideEnabled: true

    property real insetX: 0
    property real insetY: 0
    property color fillColor: Theme.hoverOnOverlay
    property color dangerFillColor: Theme.dangerSoft
    readonly property Item currentRow: _on ? _row : null

    // Bind the host's contentY so scrolling under a still pointer re-resolves.
    property real scrollOffset: 0
    onScrollOffsetChanged: refresh()

    readonly property bool active: glideEnabled && _on

    // Only matters in the wrap form, where the wrapper takes the column's
    // layout slot.
    implicitWidth: target ? target.implicitWidth : 0
    implicitHeight: target ? target.implicitHeight : 0
    z: Theme.zBelow

    property bool _on: false
    property bool _armed: false
    property bool _danger: false
    property Item _row: null
    property real _glideY: 0
    property real _glideH: 0
    property point _lastPos: Qt.point(0, 0)

    readonly property bool reduceMotion: Appearance.reduceMotion
    readonly property real renderDpr: PixelGrid.safeDpr(Screen.devicePixelRatio)

    // Disarmed (_armed false) the next placement snaps instead of flying in
    // from a stale rect.
    function reset() {
        _on = false
        _armed = false
    }

    function refresh() {
        if (glideHover.hovered)
            _track(_lastPos)
    }

    function resolveIn(item, pos) {
        if (item.glideRow === true)
            return item
        if (item.glideRow === false)
            return null
        return _descend(item, pos)
    }

    // Topmost first, and every child containing pos: an overlay stacked on a
    // row (a Canvas, a trailing chip) resolves to nothing and falls through
    // to the sibling below instead of ending the search.
    function _descend(item, pos) {
        const n = item.children.length
        const kids = []
        for (let i = 0; i < n; ++i)
            kids.push({ node: item.children[i], idx: i })
        kids.sort(function(a, b) { return b.node.z - a.node.z || b.idx - a.idx })
        for (let i = 0; i < kids.length; ++i) {
            const k = kids[i].node
            if (!k.visible)
                continue
            const sub = k.mapFromItem(item, pos)
            if (!k.contains(sub))
                continue
            if (k.glideRow === true)
                return k
            if (k.glideRow === false)
                return null
            const hit = _descend(k, sub)
            if (hit)
                return hit
        }
        return null
    }

    function _track(pos) {
        const row = rowAt ? rowAt(pos) : _descend(target, pos)
        _lastPos = pos
        if (row === null)
            return
        _glideY = row.mapToItem(root, 0, 0).y
        _glideH = row.height
        _danger = row.glideDanger === true
        _row = row
        _on = true
        if (!_armed)
            Qt.callLater(function() { root._armed = true })
    }

    readonly property var glideContext: glideEnabled ? root : null

    CrispSurface {
        id: pill
        x: (root.target ? root.target.mapToItem(root, 0, 0).x : 0)
           + root.insetX
        y: root._glideY + root.insetY
        // The row's width: delegates may be narrower than the container.
        width: (root._row ? root._row.width
                          : (root.target ? root.target.width : 0)) - root.insetX * 2
        height: Math.max(0, root._glideH - root.insetY * 2)
        visible: root.glideEnabled && (root._on || opacity > 0)
        devicePixelRatio: root.renderDpr
        radius: Theme.radiusSm
        fillEnabled: true
        fillTopColor: root._danger ? root.dangerFillColor : root.fillColor
        fillMiddleColor: fillTopColor
        fillBottomColor: fillTopColor
        strokeEnabled: false
        shadowEnabled: false
        opacity: root._on ? 1 : 0

        // NumberAnimation, not YAnimator: an Animator leaves the QML y at the
        // old endpoint until it finishes, so a retarget mid-glide would jump.
        Behavior on y {
            enabled: root._armed && !root.reduceMotion
            NumberAnimation {
                duration: Theme.durFast
                easing.type: Easing.BezierSpline
                easing.bezierCurve: Theme.easeEmphasized
            }
        }
        Behavior on opacity {
            enabled: !root.reduceMotion
            OpacityAnimator { duration: Theme.durFast }
        }
    }

    // Handlers get events through their parent item (`target` only remaps),
    // so it is parented to the container to receive target coordinates.
    HoverHandler {
        id: glideHover
        parent: root.target
        enabled: root.glideEnabled
        acceptedDevices: PointerDevice.Mouse | PointerDevice.TouchPad
        onPointChanged: root._track(point.position)
        // A menu opening under a still cursor gets no point change.
        onHoveredChanged: {
            if (hovered)
                root._track(point.position)
            else {
                root._on = false
                root._armed = false
            }
        }
    }
}
