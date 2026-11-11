import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import SpanTui

AppDialog {
    id: dialog

    property string confirmLabel: qsTr("OK")
    property var callback: null

    function openFor(heading, placeholder, confirmLabel, callback, initialText) {
        dialog.heading = heading
        input.placeholderText = placeholder
        dialog.confirmLabel = confirmLabel
        dialog.callback = callback
        input.text = initialText || ""
        open()
    }

    function accept() {
        const name = input.text.trim()
        if (name.length === 0)
            return
        close()
        if (callback)
            callback(name)
    }

    // Compact identity prompt: sm tier with the accent signature rim.
    size: "sm"
    accentRim: true
    confirmText: dialog.confirmLabel
    confirmKind: "primary"
    confirmEnabled: input.text.trim().length > 0

    onCancelled: dialog.close()
    onConfirmed: dialog.accept()
    onOpened: {
        input.forceActiveFocus()
        input.selectAll()
    }

    UiField {
        id: input
        Layout.fillWidth: true
        textSize: "md"
        onAccepted: dialog.accept()
    }
}
