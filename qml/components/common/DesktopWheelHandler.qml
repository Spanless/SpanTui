import QtQuick
import SpanTui

WheelHandler {
    id: handler
    required property var viewport
    property real lineExtent: Theme.sizeSm
    property int wheelScrollLines: 3

    target: null
    acceptedDevices: PointerDevice.Mouse | PointerDevice.TouchPad
    acceptedModifiers: Qt.KeyboardModifierMask
    // Exclusive grab only after this handler accepts. Nested viewports under
    // the pointer consume first; a bound chains to an ancestor inside
    // ScrollPolicy.handleWheel. blocking:false would scroll parent and child
    // together.
    blocking: true
    onWheel: event => {
        event.accepted = ScrollPolicy.handleWheel(
            viewport, event, lineExtent, wheelScrollLines, parent)
    }
}
