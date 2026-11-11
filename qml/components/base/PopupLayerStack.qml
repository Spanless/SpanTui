pragma Singleton
import QtQuick

// Ordered mirror of the overlay's popup stacking (open order = z order).
//
// Qt has no per-layer outside press: Popup.CloseOnPressOutside makes every
// flagged popup in the stack judge "outside" on the same press, so one click
// tears down several layers at once. Popups therefore carry the flag only
// while they are topmost here — an outside press unwinds exactly one layer.
//
// Transient ToolTips intentionally stay out of the stack: they own no
// outside-press semantics of their own worth isolating.
QtObject {
    id: root

    // Topmost open popup; null when the stack is empty. Bindings that compare
    // against it re-evaluate on every push/remove.
    readonly property var top: _stack.length ? _stack[_stack.length - 1] : null
    property var _stack: []

    function push(popup) {
        if (_stack.indexOf(popup) >= 0)
            return
        _stack = _stack.concat(popup)
    }

    function remove(popup) {
        if (_stack.indexOf(popup) < 0)
            return
        _stack = _stack.filter(entry => entry !== popup)
    }
}
