import QtQuick
import SpanTui

// Status badge: a filled disc in the status color with its symbol cut out
// (status-success / -error / -warning / -info, docs/ui/icons.md), or a
// spinner for "busy". The one way to mark success, failure, warning and
// information in the chrome.
//
// Entrance: whenever the badge appears or its kind changes, the disc pops in
// and the symbol grows out (the icon's active layer). The active pose is the
// rest shape, so the flag drops again afterwards and the icon goes back to
// its single static path. Hovering plays each kind's own gesture.
Item {
    id: root

    // "success" | "error" | "warning" | "info" | "busy"
    property string kind: "info"
    property real size: Theme.iconMd

    readonly property color toneColor: kind === "success" ? Theme.statusSuccess
                                     : kind === "error" ? Theme.statusError
                                     : kind === "warning" ? Theme.statusWarning
                                     : Theme.statusInfo

    implicitWidth: size
    implicitHeight: size

    function replay() {
        badge.active = false
        if (root.kind !== "busy" && root.visible)
            entrance.restart()
    }

    onKindChanged: replay()
    onVisibleChanged: replay()
    Component.onCompleted: replay()

    // One tick between dropping and raising the flag, so a replay is an
    // off→on edge the icon sees; then hold until the entrance has played.
    Timer {
        id: entrance
        interval: 0
        onTriggered: {
            badge.active = true
            settle.restart()
        }
    }

    Timer {
        id: settle
        interval: Theme.durSlow * 2
        onTriggered: badge.active = false
    }

    HoverHandler { id: hover }

    Icon {
        id: badge
        anchors.centerIn: parent
        visible: root.kind !== "busy"
        // No glyph behind the spinner: there is no status-busy icon.
        name: root.kind === "busy" ? "" : "status-" + root.kind
        size: root.size
        color: root.toneColor
        hovered: hover.hovered
        active: false
    }

    LoadingSpinner {
        anchors.centerIn: parent
        visible: root.kind === "busy"
        active: visible
        size: root.size
    }
}
