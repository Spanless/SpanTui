pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Layouts
import SpanTui

// Shortcuts section: every configurable command from the shortcut registry,
// grouped by where it applies, plus the fixed keys that cannot be rebound.
// The registry validates each recording; a rejection stays on that row's
// hint line until the next attempt.
SettingsSection {
    id: root

    // Action id -> rejection message for the last attempt on that row.
    property var errors: ({})
    readonly property var catalog: {
        Shortcuts.revision
        return Shortcuts.catalog()
    }
    readonly property var groups: ["terminal", "editor", "git"]

    function actionTitle(id) {
        switch (id) {
        case "terminal.find": return qsTr("Find in terminal")
        case "terminal.copy": return qsTr("Copy selection")
        case "terminal.paste": return qsTr("Paste")
        case "terminal.previousPrompt": return qsTr("Previous prompt")
        case "terminal.nextPrompt": return qsTr("Next prompt")
        case "editor.find": return qsTr("Find in file")
        case "editor.goToLine": return qsTr("Go to line")
        case "editor.copyLine": return qsTr("Copy current line")
        case "git.copyHash": return qsTr("Copy commit hash")
        }
        return id
    }

    function actionDescription(id) {
        switch (id) {
        case "terminal.find": return qsTr("Open the find bar over the terminal history and screen.")
        case "terminal.copy": return qsTr("Copy the terminal selection. Without a selection the key goes to the terminal.")
        case "terminal.paste": return qsTr("Paste the clipboard into the terminal as one bracketed paste.")
        case "terminal.previousPrompt": return qsTr("Scroll to the previous shell prompt. Needs shell integration (OSC 133); otherwise the key goes to the terminal.")
        case "terminal.nextPrompt": return qsTr("Scroll to the next shell prompt. Needs shell integration (OSC 133); otherwise the key goes to the terminal.")
        case "editor.find": return qsTr("Open the find bar in the file viewer.")
        case "editor.goToLine": return qsTr("Jump to a line number in the file viewer.")
        case "editor.copyLine": return qsTr("Copy the line under the cursor in the file viewer.")
        case "git.copyHash": return qsTr("Copy the hash of the selected commit in the history list.")
        }
        return ""
    }

    function groupTitle(group) {
        switch (group) {
        case "terminal": return qsTr("Terminal")
        case "editor": return qsTr("File viewer")
        case "git": return qsTr("Git history")
        }
        return group
    }

    function setError(id, message) {
        const next = Object.assign({}, root.errors)
        if (message.length > 0)
            next[id] = message
        else
            delete next[id]
        root.errors = next
    }

    // A shared chord is allowed: a press runs every bound action that applies.
    function sharedHint(ids) {
        if (!ids || ids.length === 0)
            return ""
        return qsTr("Shared with %1. Every action that applies runs.")
            .arg(ids.map(id => "“" + root.actionTitle(id) + "”").join(" / "))
    }

    function apply(id, sequence) {
        const result = Shortcuts.setBinding(id, sequence)
        if (result.ok) {
            root.setError(id, "")
        } else {
            root.setError(id, qsTr("Use Ctrl or Alt with a key, or a function key."))
        }
    }

    RowLayout {
        Layout.fillWidth: true
        spacing: Theme.spaceMd

        UiText {
            Layout.fillWidth: true
            text: qsTr("Click a field and press the new key combination. Backspace removes a binding, Escape cancels.")
            size: "sm"
            tone: "muted"
            wrapMode: Text.WordWrap
        }

        ChromeButton {
            size: "sm"
            iconName: "undo"
            text: qsTr("Restore all defaults")
            onClicked: {
                Shortcuts.resetAll()
                root.errors = ({})
            }
        }
    }

    Repeater {
        model: root.groups

        delegate: ColumnLayout {
            id: groupColumn

            required property string modelData

            Layout.fillWidth: true
            spacing: 0

            SectionLabel {
                text: root.groupTitle(groupColumn.modelData)
            }

            Repeater {
                model: root.catalog.filter(entry => entry.group === groupColumn.modelData)

                delegate: SettingRow {
                    id: row

                    required property var modelData

                    title: root.actionTitle(row.modelData.id)
                    description: root.actionDescription(row.modelData.id)
                    readonly property string error: root.errors[row.modelData.id] || ""

                    hint: row.error.length > 0 ? row.error : root.sharedHint(row.modelData.sharedWith)
                    hintTone: row.error.length > 0 ? "danger" : "muted"

                    ShortcutRecorder {
                        sequenceText: row.modelData.nativeText
                        invalid: row.error.length > 0
                        onRecorded: portable => root.apply(row.modelData.id, portable)
                        onCleared: root.apply(row.modelData.id, "")
                    }

                    ChromeButton {
                        size: "sm"
                        kind: "ghost"
                        iconName: "undo"
                        enabled: row.modelData.customized
                        tip: qsTr("Restore default (%1)").arg(row.modelData.defaultNativeText)
                        // Resetting rebuilds the rows; read the id first.
                        onClicked: {
                            const id = row.modelData.id
                            root.setError(id, "")
                            Shortcuts.resetBinding(id)
                        }
                    }
                }
            }
        }
    }

    SectionLabel {
        text: qsTr("Fixed keys")
    }

    Repeater {
        model: [
            { keys: "Ctrl+C", title: qsTr("Terminal: copy or interrupt"),
              description: qsTr("Copies when text is selected; otherwise sends an interrupt to the running program.") },
            { keys: "Enter / Shift+Enter", title: qsTr("Find bar: next / previous match"),
              description: qsTr("Moves between matches while the find field has focus.") },
            { keys: "Esc", title: qsTr("Find bar: close"),
              description: qsTr("Closes the find bar and returns focus to its pane.") }
        ]

        delegate: SettingRow {
            id: fixedRow
            required property var modelData

            title: modelData.title
            description: modelData.description

            UiText {
                Layout.alignment: Qt.AlignVCenter
                text: fixedRow.modelData.keys
                tone: "muted"
            }
        }
    }
}
