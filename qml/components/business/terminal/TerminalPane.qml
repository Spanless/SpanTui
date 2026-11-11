import QtQuick
import QtQuick.Window
import SpanTui

Item {
    id: root

    property color color: Theme.terminalBackdrop
    property bool interactionAnimationsEnabled: true
    property int contentLifecycle: 0
    property bool backgroundEnabled: true
    property string sessionId: ""
    readonly property bool unreadOutput: surface.unreadOutput

    function scrollToBottom() { surface.scrollToBottom() }
    function focusTerminal() { surface.focusTerminal() }

    TerminalSurface {
        id: surface
        anchors.fill: parent
        sessionId: root.sessionId
        color: root.color
        interactionAnimationsEnabled: root.interactionAnimationsEnabled
        contentLifecycle: root.contentLifecycle
        backgroundEnabled: root.backgroundEnabled
    }

    Connections {
        target: SessionModel
        function onActiveIndexChanged() {
            // Do not steal focus from editor, dialogs, or chrome on passive
            // catalog sync. Only retake keys when focus is already in this pane.
            const win = root.Window.window
            const focus = win ? win.activeFocusItem : null
            if (!focus)
                return
            let item = focus
            while (item) {
                if (item === root) {
                    surface.focusTerminal()
                    return
                }
                item = item.parent
            }
        }
    }

}
