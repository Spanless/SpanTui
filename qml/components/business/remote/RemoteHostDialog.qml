import QtQuick
import QtQuick.Dialogs
import QtQuick.Layouts
import SpanTui

// Create / edit one saved SSH host. Saved secrets never come back to QML:
// the fields only say whether one exists, an empty field keeps it and the
// clear buttons drop it; a new address, port or user drops the password. A new host connects right after saving.
AppDialog {
    id: dialog

    property string hostId: ""
    property bool hasPassword: false
    property bool hasKeyPassphrase: false
    property bool clearPassword: false
    property bool clearKeyPassphrase: false
    readonly property var fields: ({
        id: dialog.hostId,
        name: nameField.text,
        host: hostField.text,
        port: Number(portField.text),
        user: userField.text,
        identityFile: keyField.text,
        password: passwordField.text,
        clearPassword: dialog.clearPassword,
        keyPassphrase: passphraseField.text,
        clearKeyPassphrase: dialog.clearKeyPassphrase
    })
    // Pointing the host at another server drops its saved password.
    readonly property bool keepsPassword: hasPassword && !clearPassword
                                          && !RemoteHosts.movesHost(fields)
    readonly property string problem: hostField.text.trim().length === 0
                                      ? "" : RemoteHosts.validationError(fields)

    // A saved new host started its first session (Settings closes on it).
    signal sessionStarted()

    // `prefill` ({ name, host }) seeds a new host, e.g. a Tailscale device.
    function openFor(id, prefill) {
        const details = id.length > 0 ? RemoteHosts.hostDetails(id) : (prefill || ({}))
        hostId = details.id || ""
        nameField.text = details.name || ""
        hostField.text = details.host || ""
        portField.text = String(details.port || 22)
        userField.text = details.user || ""
        keyField.text = details.identityFile || ""
        passwordField.text = ""
        passphraseField.text = ""
        hasPassword = details.hasPassword === true
        hasKeyPassphrase = details.hasKeyPassphrase === true
        clearPassword = false
        clearKeyPassphrase = false
        open()
    }

    function accept() {
        if (!confirmEnabled)
            return
        const isNew = hostId.length === 0
        const saved = RemoteHosts.saveHost(fields)
        if (saved.length === 0)
            return
        close()
        if (isNew) {
            WorkspaceController.createRemoteSession(saved)
            sessionStarted()
        }
    }

    heading: hostId.length > 0 ? qsTr("Edit SSH host") : qsTr("New SSH host")
    size: "md"
    accentRim: true
    confirmText: hostId.length > 0 ? qsTr("Save") : qsTr("Save and connect")
    confirmEnabled: hostField.text.trim().length > 0 && problem.length === 0

    onCancelled: dialog.close()
    onConfirmed: dialog.accept()
    onOpened: (hostField.text.length > 0 ? nameField : hostField).forceActiveFocus()

    GridLayout {
        Layout.fillWidth: true
        columns: 2
        columnSpacing: Theme.spaceMd
        rowSpacing: Theme.spaceMd

        UiText { text: qsTr("Host"); tone: "muted" }
        RowLayout {
            Layout.fillWidth: true
            spacing: Theme.spaceSm
            UiField {
                id: hostField
                Layout.fillWidth: true
                placeholderText: qsTr("example.com or 192.168.1.10")
                onAccepted: dialog.accept()
            }
            UiField {
                id: portField
                Layout.preferredWidth: Theme.sizeLg * 2
                role: "mono"
                validator: IntValidator { bottom: 1; top: 65535 }
                onAccepted: dialog.accept()
            }
        }

        UiText { text: qsTr("User"); tone: "muted" }
        UiField {
            id: userField
            Layout.fillWidth: true
            placeholderText: qsTr("root")
            onAccepted: dialog.accept()
        }

        UiText { text: qsTr("Name"); tone: "muted" }
        UiField {
            id: nameField
            Layout.fillWidth: true
            placeholderText: hostField.text.length > 0 ? hostField.text : qsTr("Shown in the sidebar")
            onAccepted: dialog.accept()
        }

        UiText { text: qsTr("Password"); tone: "muted" }
        UiField {
            id: passwordField
            Layout.fillWidth: true
            echoMode: TextInput.Password
            placeholderText: dialog.keepsPassword
                             ? qsTr("Saved — leave empty to keep")
                             : qsTr("Optional, saved encrypted")
            onTextChanged: if (text.length > 0) dialog.clearPassword = false
            onAccepted: dialog.accept()
            trailing: ChromeButton {
                visible: dialog.keepsPassword
                kind: "ghost"
                size: "sm"
                iconName: "eraser"
                tip: qsTr("Forget saved password")
                onClicked: dialog.clearPassword = true
            }
        }

        UiText { text: qsTr("Private key"); tone: "muted" }
        UiField {
            id: keyField
            Layout.fillWidth: true
            role: "mono"
            fadeOverflow: true
            placeholderText: qsTr("Optional — ssh-agent and default keys otherwise")
            onAccepted: dialog.accept()
            trailing: ChromeButton {
                kind: "ghost"
                size: "sm"
                iconName: "folder"
                tip: qsTr("Choose private key file")
                onClicked: keyDialog.open()
            }
        }

        Item { visible: keyHint.visible }
        UiText {
            id: keyHint
            visible: keyField.text.length > 0
            Layout.fillWidth: true
            text: qsTr("Saving imports a copy only you can read, as OpenSSH requires. The original file is not changed.")
            tone: "muted"
            size: "sm"
            wrapMode: Text.WordWrap
        }

        UiText {
            visible: keyField.text.length > 0
            text: qsTr("Key passphrase")
            tone: "muted"
        }
        UiField {
            id: passphraseField
            visible: keyField.text.length > 0
            Layout.fillWidth: true
            echoMode: TextInput.Password
            placeholderText: dialog.hasKeyPassphrase && !dialog.clearKeyPassphrase
                             ? qsTr("Saved — leave empty to keep")
                             : qsTr("Optional, saved encrypted")
            onTextChanged: if (text.length > 0) dialog.clearKeyPassphrase = false
            onAccepted: dialog.accept()
            trailing: ChromeButton {
                visible: dialog.hasKeyPassphrase && !dialog.clearKeyPassphrase
                kind: "ghost"
                size: "sm"
                iconName: "eraser"
                tip: qsTr("Forget saved passphrase")
                onClicked: dialog.clearKeyPassphrase = true
            }
        }
    }

    UiText {
        Layout.fillWidth: true
        visible: dialog.problem.length > 0
        text: dialog.problem
        tone: "danger"
        size: "sm"
        wrapMode: Text.WordWrap
    }

    FileDialog {
        id: keyDialog
        title: qsTr("Choose private key file")
        fileMode: FileDialog.OpenFile
        onAccepted: keyField.text = RemoteHosts.keyPathFromUrl(selectedFile)
    }
}
