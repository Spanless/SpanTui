import QtQuick
import QtQuick.Layouts
import SpanTui

// Records one key chord for a shortcut binding. Focus arms recording and
// the next chord is reported through `recorded`; the owner decides whether
// to accept it, so `sequenceText` only changes through the registry.
// While recording every key is claimed through ShortcutOverride, so window
// shortcuts and the sheet's Escape never fire underneath. Escape cancels,
// Backspace/Delete clears the binding, and plain Tab keeps moving focus.
UiField {
    id: root

    // Display (native) text of the current binding.
    property string sequenceText: ""
    readonly property bool recording: activeFocus

    signal recorded(string portable)
    signal cleared()

    function finish() {
        root.focus = false
    }

    function isPlainTab(event) {
        return (event.key === Qt.Key_Tab || event.key === Qt.Key_Backtab)
               && (event.modifiers & (Qt.ControlModifier | Qt.AltModifier)) === 0
    }

    Layout.preferredWidth: Theme.fieldMd
    Layout.alignment: Qt.AlignVCenter
    readOnly: true
    leadingIcon: "keyboard"
    text: root.recording ? "" : root.sequenceText
    placeholderText: root.recording ? qsTr("Press a key combination…") : qsTr("Not set")
    Accessible.description: qsTr("Focus the field and press the new key combination")

    Keys.onShortcutOverride: event => event.accepted = !root.isPlainTab(event)
    Keys.onPressed: event => {
        if (root.isPlainTab(event))
            return
        event.accepted = true
        const plain = (event.modifiers & (Qt.ControlModifier | Qt.AltModifier
                                          | Qt.ShiftModifier | Qt.MetaModifier)) === 0
        if (plain && event.key === Qt.Key_Escape) {
            root.finish()
            return
        }
        // Finish before reporting: applying a binding rebuilds the owner's
        // rows, which can destroy this field inside the signal.
        if (plain && (event.key === Qt.Key_Backspace || event.key === Qt.Key_Delete)) {
            root.finish()
            root.cleared()
            return
        }
        const sequence = Shortcuts.sequenceFromKey(event.key, event.modifiers)
        if (sequence.length === 0)
            return
        root.finish()
        root.recorded(sequence)
    }
}
