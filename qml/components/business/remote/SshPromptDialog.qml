import QtQuick
import QtQuick.Layouts
import SpanTui

// Presents RemoteHostRegistry.pendingPrompt — an ssh/sftp question nobody
// could answer from the saved or already typed secrets: a password or key
// passphrase, an unknown host key, or another challenge. One prompt at a
// time; the registry queues the rest and answers duplicates itself.
AppDialog {
    id: dialog

    property var prompt: ({})
    property bool remember: true
    readonly property string kind: prompt.kind || ""
    readonly property bool secret: kind === "password" || kind === "passphrase" || kind === "secret"
    readonly property bool hostKey: kind === "hostKey"
    // False for one-time answers and for secrets the registry cannot tie to
    // this host's own login (a ProxyJump hop): they are used once, never saved.
    readonly property bool canRemember: secret && prompt.canRemember === true

    function sync() {
        const next = RemoteHosts.pendingPrompt
        if (!next.requestId) {
            if (opened)
                close()
            return
        }
        if (opened && next.requestId === prompt.requestId)
            return
        prompt = next
        input.text = ""
        remember = true
        open()
    }

    function answer(accepted) {
        const id = prompt.requestId
        if (!id)
            return
        const text = hostKey ? "yes" : input.text
        const save = accepted && canRemember && remember
        prompt = ({})
        close()
        RemoteHosts.answerPrompt(id, accepted, accepted ? text : "", save)
    }

    heading: hostKey ? qsTr("Trust this host?")
             : kind === "passphrase" ? qsTr("Private key passphrase")
             : kind === "password" ? qsTr("SSH password")
             : qsTr("SSH login")
    message: prompt.hostName ? prompt.hostName + "  ·  " + prompt.address : ""
    size: hostKey ? "lg" : "md"
    accentRim: !hostKey
    confirmText: hostKey ? qsTr("Trust and connect") : qsTr("Connect")
    cancelText: hostKey ? qsTr("Reject") : qsTr("Cancel")
    confirmEnabled: hostKey || input.text.length > 0

    onConfirmed: answer(true)
    onCancelled: answer(false)
    // Escape or any other dismissal refuses, so ssh never waits forever.
    onClosed: if (prompt.requestId) answer(false)
    onOpened: if (!hostKey) input.forceActiveFocus()

    Connections {
        target: RemoteHosts
        function onPendingPromptChanged() { Qt.callLater(dialog.sync) }
    }
    Component.onCompleted: sync()

    InlineBanner {
        Layout.fillWidth: true
        active: dialog.prompt.retry === true
        tone: "warning"
        text: qsTr("The previous answer was rejected. Try again.")
    }

    // ssh's own wording: for a host key it carries the fingerprint.
    MonoText {
        Layout.fillWidth: true
        text: dialog.prompt.text || ""
        wrapMode: Text.WrapAnywhere
        color: Theme.muted
    }

    UiField {
        id: input
        Layout.fillWidth: true
        visible: !dialog.hostKey
        echoMode: dialog.secret ? TextInput.Password : TextInput.Normal
        onAccepted: if (dialog.confirmEnabled) dialog.answer(true)
    }

    AppSwitch {
        visible: dialog.canRemember
        checked: dialog.remember
        text: dialog.kind === "passphrase" ? qsTr("Remember passphrase")
                                           : qsTr("Remember password")
        onToggled: newChecked => dialog.remember = newChecked
    }
}
