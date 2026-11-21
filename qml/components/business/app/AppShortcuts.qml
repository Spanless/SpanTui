import QtQml
import QtQuick
import SpanTui

// The application commands as window shortcuts where there is no system
// menu bar to carry them (Windows). A focused terminal leaves these chords
// alone (TerminalView.applicationBindings).
Instantiator {
    id: root

    required property AppCommands commands

    model: root.commands.ids
    delegate: Shortcut {
        id: shortcut
        required property string modelData
        sequence: {
            const _ = Shortcuts.revision
            return Shortcuts.sequence(shortcut.modelData)
        }
        enabled: sequence.length > 0
        onActivated: root.commands.run(shortcut.modelData)
    }
}
