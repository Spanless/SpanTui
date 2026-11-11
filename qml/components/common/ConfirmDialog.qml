import QtQuick
import SpanTui

AppDialog {
    id: root

    property bool danger: true
    property var callback: null

    function openFor(titleText, bodyText, actionText, action) {
        heading = titleText
        message = bodyText
        confirmText = actionText
        callback = action
        open()
    }

    confirmText: qsTr("Confirm")
    confirmKind: danger ? "danger" : "primary"

    onCancelled: root.close()
    onConfirmed: {
        const action = root.callback
        root.callback = null
        root.close()
        if (action) action()
    }
    // Any other dismissal drops the pending confirmation.
    onClosed: callback = null
}
