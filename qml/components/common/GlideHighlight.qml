import QtQuick
import SpanTui

// One hover pill shared by a container's rows instead of each row painting
// its own hover. `target` is the row container: hit tests and the pill frame
// map through it.
//
// Motion: the pill is the hovered row's rect plus an offset, and only the
// offset moves. A row change keeps the pill where it is drawn and the offset
// decays exponentially each frame (time constant followTau), so x, y, width
// and height travel together and a retarget mid-glide carries on from the
// drawn position instead of restarting a fixed-length animation — that
// restart is what left a Behavior-driven pill trailing a fast pointer.
// Entering is instant, at the row — also while a previous exit still fades,
// so the pill never slides in from where the pointer left. Leaving the
// container fades out briefly (fadeOutMs).
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

    // Glide time constant (≈17ms): ~4τ (≈ half of durFast) to settle within
    // half a physical pixel; at constant pointer speed the pill trails by
    // speed × τ — about one frame.
    readonly property real followTau: Theme.durFast / 8
    // A frame longer than this (a stall) advances the glide no further, so the
    // pill does not jump the whole way in one step after a hitch.
    readonly property real followMaxStepMs: Theme.durFast / 2
    // Short enough that the pill is gone by the time the pointer lands on
    // whatever lies outside the container.
    readonly property int fadeOutMs: Theme.durHover

    property bool _on: false
    // Set by reset(): the rows changed, so the next placement must not glide
    // from a rect that belonged to the old rows.
    property bool _stale: true
    property bool _danger: false
    property Item _row: null
    property real _glideX: 0
    property real _glideY: 0
    property real _glideH: 0
    property point _lastPos: Qt.point(0, 0)
    // Drawn rect minus the row rect; decays to zero while gliding.
    property real _offX: 0
    property real _offY: 0
    property real _offW: 0
    property real _offH: 0

    readonly property bool reduceMotion: Appearance.reduceMotion
    readonly property real renderDpr: PixelGrid.safeDpr(Screen.devicePixelRatio)

    // Stale first: the _on handler then hides the pill at once instead of
    // fading an old row's rect.
    function reset() {
        _stale = true
        _on = false
    }

    // Appearing is instant; only disappearing fades. Imperative because a
    // Behavior's `enabled` and the value binding both depend on `_on` and
    // their re-evaluation order is unspecified.
    on_OnChanged: {
        if (_on) {
            fadeOut.stop()
            pill.opacity = 1
        } else if (reduceMotion || !glideEnabled || _stale) {
            fadeOut.stop()
            pill.opacity = 0
        } else {
            fadeOut.restart()
        }
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
        const origin = row.mapToItem(root, 0, 0)
        // Glide only between rows of one continuous hover over these rows.
        const glide = _on && _row !== null && !_stale && !reduceMotion
        const fromX = pill.x
        const fromY = pill.y
        const fromW = pill.width
        const fromH = pill.height
        _glideX = origin.x
        _glideY = origin.y
        _glideH = row.height
        _danger = row.glideDanger === true
        _row = row
        _stale = false
        if (glide) {
            // The bindings already moved the pill to the new row plus the
            // old offset; fold the jump back into the offset.
            _offX += fromX - pill.x
            _offY += fromY - pill.y
            _offW += fromW - pill.width
            _offH += fromH - pill.height
            if (!_settled())
                follower.start()
        } else {
            _settle()
        }
        _on = true
    }

    function _settled() {
        const rest = 0.5 / renderDpr // style-ok: half a physical pixel
        return Math.abs(_offX) < rest && Math.abs(_offY) < rest
               && Math.abs(_offW) < rest && Math.abs(_offH) < rest
    }

    function _settle() {
        follower.stop()
        _offX = 0
        _offY = 0
        _offW = 0
        _offH = 0
    }

    readonly property var glideContext: glideEnabled ? root : null

    CrispSurface {
        id: pill
        // The row's own rect: delegates may be indented or narrower than the
        // container (worktree sessions in the sidebar).
        x: (root._row ? root._glideX
                      : (root.target ? root.target.mapToItem(root, 0, 0).x : 0))
           + root.insetX + root._offX
        y: root._glideY + root.insetY + root._offY
        width: (root._row ? root._row.width
                          : (root.target ? root.target.width : 0)) - root.insetX * 2
               + root._offW
        height: Math.max(0, root._glideH - root.insetY * 2 + root._offH)
        visible: root.glideEnabled && (root._on || opacity > 0)
        devicePixelRatio: root.renderDpr
        radius: Theme.radiusSm
        fillEnabled: true
        fillTopColor: root._danger ? root.dangerFillColor : root.fillColor
        fillMiddleColor: fillTopColor
        fillBottomColor: fillTopColor
        strokeEnabled: false
        shadowEnabled: false
        opacity: 0
    }

    // Runs only while an offset is left; exponential decay is frame-rate
    // independent and has no fixed length to restart.
    FrameAnimation {
        id: follower
        onTriggered: {
            const ms = Math.min(frameTime * 1000, root.followMaxStepMs)
            const keep = Math.exp(-ms / root.followTau)
            root._offX *= keep
            root._offY *= keep
            root._offW *= keep
            root._offH *= keep
            if (root._settled())
                root._settle()
        }
    }

    NumberAnimation {
        id: fadeOut
        target: pill
        property: "opacity"
        to: 0
        duration: root.fadeOutMs
        easing.type: Easing.OutCubic
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
            else
                root._on = false
        }
    }
}
