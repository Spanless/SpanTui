import QtQuick
import SpanTui

// Cache-style expand host: children stay alive; only the clip height animates.
// Avoids destroy/recreate (which breaks rapid toggle and steals clicks).
//
// reveal uses an explicit NumberAnimation (not Behavior alone) so mid-flight
// reverse on the same item always restarts from the current height instead of
// waiting out a queued Behavior. The content rides a small downward unfold
// with the clip so children read as dropping into place. Reduced motion snaps.
Item {
    id: root

    property bool expanded: false
    property bool hasContent: true
    property int spacing: 0
    default property alias contentData: content.data

    readonly property bool open: expanded && hasContent
    readonly property real contentHeight: Math.max(0, content.implicitHeight)

    // 0 = fully collapsed, 1 = fully open. Driven by openChanged below.
    property real reveal: 0

    width: parent ? parent.width : 0
    clip: true
    height: reveal * contentHeight
    implicitHeight: height
    implicitWidth: width
    // Keep item in the scene even when collapsed (cache). Block input only.
    enabled: height > 1

    onOpenChanged: {
        // Restart from the live value so rapid open/close never waits for a
        // previous OutQuint to finish (that felt like a 1–2s dead chevron).
        revealAnim.stop()
        if (Appearance.reduceMotion) {
            reveal = open ? 1 : 0
            return
        }
        revealAnim.from = reveal
        revealAnim.to = open ? 1 : 0
        revealAnim.start()
    }

    Component.onCompleted: {
        // First paint: jump to the correct state without animating in.
        reveal = open ? 1 : 0
    }

    NumberAnimation {
        id: revealAnim
        target: root
        property: "reveal"
        duration: Theme.durFast
        easing.type: Easing.BezierSpline
        easing.bezierCurve: Theme.easeEmphasized
    }

    Column {
        id: content
        width: parent.width
        spacing: root.spacing
        // Unfold: the rows start a small step above their slot and settle as
        // the clip opens.
        y: -(1 - root.reveal) * Theme.spaceSm
        // Always fully laid out so contentHeight is stable while clipped.
        opacity: root.height > 0.5 ? 1 : 0 // style-ok: half-grown collapse threshold
    }
}
