import QtQuick
import QtQuick.Layouts
import SpanTui

// Terminal tiles have a different close contract from ordinary content: the
// container may disappear while the session remains available in the sidebar.
AppDialog {
    id: root

    property bool terminateSession: true
    property var callback: null

    function openFor(defaultTerminateSession, action) {
        terminateSession = defaultTerminateSession
        callback = action
        open()
    }

    heading: qsTr("Close terminal pane")
    message: qsTr("This closes the content pane. You can keep the session and reopen it from the sidebar.")
    confirmText: qsTr("Close pane")
    confirmKind: "danger"

    onCancelled: root.close()
    onConfirmed: {
        const action = root.callback
        const terminate = root.terminateSession
        root.close()
        if (action)
            action(terminate)
    }

    AppSwitch {
        Layout.fillWidth: true
        text: qsTr("Also terminate the terminal session")
        checked: root.terminateSession
        onToggled: function(newChecked) { root.terminateSession = newChecked }
    }
}
