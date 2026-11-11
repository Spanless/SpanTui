pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Layouts
import SpanTui

// Remote section: how ssh's questions reach SpanTui (the prompt bridge) and
// the devices of the local Tailscale network. A tailnet device is added as
// an ordinary saved host whose address is its Tailscale IP, so ssh reaches
// it straight over the tailnet; nothing Tailscale-specific is stored. The
// device list is read when the section shows and on Refresh, never polled.
SettingsSection {
    id: root

    // Starting a session from here leaves Settings for the workspace.
    signal closeRequested()

    function tailscaleHint() {
        switch (TailscaleDevices.state) {
        case "unavailable":
            return qsTr("tailscale.exe not found. Install Tailscale, or choose it under Environment.")
        case "idle":
            return qsTr("Checking…")
        case "needsLogin":
            return qsTr("Signed out. Sign in from the Tailscale tray icon.")
        case "stopped":
            return qsTr("Disconnected. Connect from the Tailscale tray icon.")
        case "starting":
            return qsTr("Tailscale is starting.")
        case "error":
            return TailscaleDevices.error.length > 0
                   ? qsTr("Could not read the tailnet: %1").arg(TailscaleDevices.error)
                   : qsTr("Could not read the tailnet.")
        }
        return TailscaleDevices.tailnet.length > 0
               ? qsTr("Connected to %1 · %n device(s)", "", TailscaleDevices.deviceCount)
                   .arg(TailscaleDevices.tailnet)
               : qsTr("Connected · %n device(s)", "", TailscaleDevices.deviceCount)
    }

    function sshText(state) {
        switch (state) {
        case "tailscale": return qsTr("Tailscale SSH")
        case "open": return qsTr("SSH port open")
        case "closed": return qsTr("No SSH")
        case "probing": return qsTr("Checking SSH…")
        case "offline": return qsTr("Offline")
        }
        return ""
    }

    onVisibleChanged: if (visible) TailscaleDevices.refresh()

    SectionLabel {
        text: qsTr("SSH")
    }

    SettingRow {
        visible: !Tools.sshAvailable
        title: qsTr("OpenSSH client")
        description: qsTr("Remote hosts run the system ssh.exe. Turn on the Windows optional feature \"OpenSSH Client\", or choose ssh.exe under Environment.")
        hint: qsTr("Not available.")
        hintTone: "warning"
    }

    // Not a tool: the pipe through which ssh's questions reach SpanTui
    // dialogs. Only meaningful while ssh itself is available.
    SettingRow {
        visible: Tools.sshAvailable
        title: qsTr("SSH prompt bridge")
        description: qsTr("Passes password, key passphrase and host key questions from ssh and sftp to SpanTui dialogs. While it is not running, remote terminals ask in the terminal and remote file browsing needs key or agent login; restart SpanTui to try again.")
        hint: RemoteHosts.promptBridgeAvailable
              ? qsTr("Running.")
              : qsTr("Not running: %1")
                  .arg(RemoteHosts.promptBridgeError.length > 0 ? RemoteHosts.promptBridgeError
                                                                : qsTr("unknown error"))
        hintTone: RemoteHosts.promptBridgeAvailable ? "success" : "warning"

        // The local pipe ssh's prompts travel through; read-only.
        UiField {
            Layout.preferredWidth: Theme.fieldMd
            Layout.alignment: Qt.AlignVCenter
            role: "mono"
            fadeOverflow: true
            readOnly: true
            text: RemoteHosts.promptBridgePipe
            placeholderText: qsTr("Not running")
            leading: StatusIcon {
                kind: RemoteHosts.promptBridgeAvailable ? "success" : "warning"
                size: Theme.iconSm
            }
        }
    }

    SectionLabel {
        text: qsTr("Tailscale")
    }

    // The status sentence is too long for a hint line (capped at the control
    // width), so it reads inside a read-only field like the bridge pipe.
    SettingRow {
        title: qsTr("Tailnet")
        description: qsTr("Lists the devices of your Tailscale network. Adding one saves it as an SSH host at its Tailscale address, so connections go straight over the tailnet. Devices running Tailscale SSH need no password or key.")

        UiField {
            Layout.preferredWidth: Theme.fieldMd
            Layout.alignment: Qt.AlignVCenter
            fadeOverflow: true
            readOnly: true
            text: root.tailscaleHint()
            leading: StatusIcon {
                kind: TailscaleDevices.refreshing || TailscaleDevices.state === "idle"
                      || TailscaleDevices.state === "starting" ? "busy"
                      : TailscaleDevices.state === "running" ? "success" : "warning"
                size: Theme.iconSm
            }
            trailing: ChromeButton {
                kind: "ghost"
                size: "sm"
                iconName: "refresh"
                tip: qsTr("Refresh")
                enabled: !TailscaleDevices.refreshing
                onClicked: TailscaleDevices.refresh()
            }
        }
    }

    SettingRow {
        visible: TailscaleDevices.state === "running"
        title: qsTr("Only SSH devices")
        description: qsTr("Hide devices that are offline or do not accept SSH. A device counts when it runs Tailscale SSH or its port 22 answers over the tailnet.")

        // The progress / empty note rides as the switch label: a hint line
        // would be capped at the switch track's width.
        AppSwitch {
            Layout.alignment: Qt.AlignVCenter
            checked: TailscaleDevices.sshOnly
            text: TailscaleDevices.probingCount > 0
                  ? qsTr("Checking SSH on %n device(s)…", "", TailscaleDevices.probingCount)
                  : TailscaleDevices.deviceCount === 0
                    ? qsTr("No other devices in the tailnet.")
                    : deviceRepeater.count === 0 ? qsTr("No device accepts SSH.") : ""
            onToggled: newChecked => TailscaleDevices.sshOnly = newChecked
        }
    }

    Repeater {
        id: deviceRepeater
        model: TailscaleDevices.state === "running" ? TailscaleDevices : null

        delegate: SettingRow {
            id: deviceRow
            required property string name
            required property string hostName
            required property string dnsName
            required property string address
            required property string os
            required property bool online
            required property string sshState
            // Re-read whenever the saved hosts change (count notifies on
            // every save and removal).
            readonly property string savedId:
                (RemoteHosts.count, RemoteHosts.hostIdForAddress([address, dnsName]))

            // Title: the tailnet's machine name. The ⓘ carries the SSH state
            // and the other names; the control shows only the address.
            title: name
            description: [root.sshText(sshState), dnsName,
                          hostName !== name ? hostName : "", os]
                         .filter(part => part.length > 0).join(" · ")

            // Display only: the address is not a field to edit or scroll.
            RowLayout {
                Layout.preferredWidth: Theme.fieldMd
                Layout.alignment: Qt.AlignVCenter
                spacing: Theme.spaceSm

                StatusIcon {
                    Layout.alignment: Qt.AlignVCenter
                    kind: deviceRow.sshState === "tailscale" || deviceRow.sshState === "open"
                          ? "success" : deviceRow.sshState === "probing" ? "busy" : "warning"
                    size: Theme.iconSm
                }

                MonoText {
                    Layout.fillWidth: true
                    Layout.alignment: Qt.AlignVCenter
                    text: deviceRow.address
                    elide: Text.ElideRight
                }

                ChromeButton {
                    Layout.alignment: Qt.AlignVCenter
                    kind: "secondary"
                    size: "sm"
                    text: deviceRow.savedId.length > 0 ? qsTr("Connect") : qsTr("Add…")
                    enabled: Tools.sshAvailable && deviceRow.online
                             && deviceRow.address.length > 0
                    onClicked: {
                        if (deviceRow.savedId.length > 0) {
                            WorkspaceController.createRemoteSession(deviceRow.savedId)
                            root.closeRequested()
                        } else {
                            hostDialog.openFor("", { name: deviceRow.name,
                                                     host: deviceRow.address })
                        }
                    }
                }
            }
        }
    }

    RemoteHostDialog {
        id: hostDialog
        onSessionStarted: root.closeRequested()
    }
}
