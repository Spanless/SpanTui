import QtQuick

// CloseOnPressOutside closes on the press; the same click then reaches the
// trigger and would reopen. A Timer, not recursive Qt.callLater: destruction
// mid-press must cancel the wait, not run closures on a dead context.
QtObject {
    id: root

    property var lastSource: null
    property bool blocked: false

    function sourceHeld(source) {
        if (!source)
            return false
        return source.down === true || source.pressed === true
    }

    function remember(source) {
        root.lastSource = source
    }

    function arm() {
        _cleanTicks = 0
        root.blocked = true
    }

    function shouldSkip(source) {
        if (!root.blocked)
            return false
        if (root.lastSource !== source)
            return false
        root.blocked = false
        return true
    }

    property int _cleanTicks: 0

    // Two released reads: the first can precede the re-delivered press.
    // A named property because QtObject has no default property.
    property Timer _disarmPoll: Timer {
        interval: 16
        repeat: true
        running: root.blocked
        onTriggered: {
            if (root.sourceHeld(root.lastSource)) {
                root._cleanTicks = 0
            } else if (++root._cleanTicks >= 2) {
                root.blocked = false
            }
        }
    }
}
