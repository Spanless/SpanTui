pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Dialogs
import QtQuick.Layouts
import SpanTui

// Environment section: the external tools SpanTui drives, whether each one
// was found and runs, and an optional executable override. A tool that is
// not available hides the features built on it (Git pages, remote hosts).
// Each tool row shows the path in use inside its field, led by a StatusIcon
// in the field's leading slot; the hint line keeps to a short status, since
// it is one line capped at the control width. Install guidance sits in the ⓘ.
// Rendering: hardware or software (WARP) graphics, fixed per launch.
SettingsSection {
    id: root

    function toolTitle(toolId) {
        switch (toolId) {
        case "git": return qsTr("Git")
        case "ssh": return qsTr("OpenSSH client")
        case "sftp": return qsTr("SFTP client")
        case "tailscale": return qsTr("Tailscale")
        }
        return toolId
    }

    function toolDescription(toolId) {
        switch (toolId) {
        case "git":
            return qsTr("git.exe powers the Changes and History pages, branch badges and change bars. Without it those features stay hidden. If it is missing, install Git for Windows.")
        case "ssh":
            return qsTr("ssh.exe connects remote hosts and their terminals. Without it the Remote section stays hidden. If it is missing, turn on the Windows optional feature \"OpenSSH Client\".")
        case "sftp":
            return qsTr("sftp.exe uploads and downloads files for remote hosts. If it is missing, turn on the Windows optional feature \"OpenSSH Client\".")
        case "tailscale":
            return qsTr("tailscale.exe lists the devices of your tailnet under Settings → Remote. Optional; without it only that list is missing. Install the Tailscale client to use it.")
        }
        return ""
    }

    // The path is in the field, so the status stays short.
    function statusText(status, version, minimumVersion) {
        switch (status) {
        case "checking":
            return qsTr("Checking…")
        case "available":
            return version.length > 0 ? qsTr("Version %1").arg(version) : qsTr("Found")
        case "missing":
            return qsTr("Not found. Install it, or choose the executable.")
        case "outdated":
            return qsTr("Version %1 is older than the required %2.")
                .arg(version.length > 0 ? version : qsTr("unknown")).arg(minimumVersion)
        case "broken":
            return qsTr("Could not run it.")
        case "invalid":
            return qsTr("Not an executable file.")
        }
        return ""
    }

    function statusKind(status) {
        return status === "available" ? "success" : status === "checking" ? "busy" : "warning"
    }

    // Hint lines are one line capped at the control width: short notes
    // only. Why this launch differs from the choice, or "" when it does not.
    function graphicsNote() {
        if (Graphics.restartRequired)
            return qsTr("Applies after restarting SpanTui.")
        switch (Graphics.softwareReason) {
        case "launchOption":
            return qsTr("This launch used --safe-graphics.")
        case "interruptedStart":
            return qsTr("Switched after the last start failed.")
        }
        return ""
    }

    function rendererText() {
        if (Graphics.rendererApi.length === 0)
            return qsTr("Starting…")
        if (Graphics.rendererSoftware)
            return qsTr("Software · %1").arg(Graphics.rendererDevice)
        return Graphics.rendererDevice.length > 0
            ? Graphics.rendererDevice + " · " + Graphics.rendererApi
            : Graphics.rendererApi
    }

    SectionLabel {
        text: qsTr("External Tools")
    }

    Repeater {
        model: Tools

        delegate: SettingRow {
            id: toolRow
            required property string toolId
            required property string status
            required property string version
            required property string path
            required property string overridePath
            required property string minimumVersion

            title: root.toolTitle(toolId)
            description: root.toolDescription(toolId)
            hint: root.statusText(status, version, minimumVersion)
            hintTone: status === "available" ? "success"
                      : status === "checking" ? "muted" : "warning"

            UiField {
                id: pathField
                Layout.preferredWidth: Theme.fieldMd
                Layout.alignment: Qt.AlignVCenter
                role: "mono"
                fadeOverflow: true
                // The path in use: the chosen one, else the detected one.
                text: toolRow.overridePath.length > 0 ? toolRow.overridePath : toolRow.path
                placeholderText: qsTr("Detect automatically")
                leading: StatusIcon {
                    kind: root.statusKind(toolRow.status)
                    size: Theme.iconSm
                }
                onEditingFinished: {
                    const value = text.trim()
                    // Leaving the detected path as shown is not a choice.
                    if (value === toolRow.overridePath
                            || (toolRow.overridePath.length === 0 && value === toolRow.path))
                        return
                    Tools.setOverridePath(toolRow.toolId, value)
                }

                trailing: Row {
                    spacing: Theme.spaceSm

                    ChromeButton {
                        kind: "ghost"
                        size: "sm"
                        iconName: "undo"
                        visible: toolRow.overridePath.length > 0
                        tip: qsTr("Detect automatically")
                        onClicked: Tools.setOverridePath(toolRow.toolId, "")
                    }

                    ChromeButton {
                        kind: "ghost"
                        size: "sm"
                        iconName: "folder"
                        tip: qsTr("Choose executable")
                        onClicked: {
                            executableDialog.toolId = toolRow.toolId
                            executableDialog.open()
                        }
                    }
                }
            }
        }
    }

    SettingRow {
        title: qsTr("Check again")
        description: qsTr("Search for the tools again after installing one or changing PATH. No restart is needed.")

        ChromeButton {
            Layout.alignment: Qt.AlignVCenter
            kind: "secondary"
            text: Tools.checking ? qsTr("Checking…") : qsTr("Check again")
            enabled: !Tools.checking
            onClicked: Tools.refresh()
        }
    }

    SectionLabel {
        text: qsTr("Rendering")
    }

    SettingRow {
        title: qsTr("Graphics")
        description: qsTr("Hardware draws with the graphics card. Software draws on the CPU: slower, but independent of the graphics driver. Use it when the window stays blank, flickers or crashes. Applies at the next start.")
        hint: root.graphicsNote()
        hintTone: Graphics.restartRequired || Graphics.softwareReason === "interruptedStart"
                  ? "warning" : "muted"

        SegmentedControl {
            Layout.preferredWidth: Theme.fieldMd
            Layout.alignment: Qt.AlignVCenter
            model: [
                { key: "auto", text: qsTr("Hardware") },
                { key: "software", text: qsTr("Software") }
            ]
            current: Graphics.mode
            onPicked: key => Graphics.mode = key
        }
    }

    SettingRow {
        title: qsTr("Current renderer")
        description: qsTr("The graphics interface and adapter this launch draws with, read after the first frame.")
        // Hardware was asked for, but Direct3D fell back by itself.
        hint: Graphics.rendererSoftware && Graphics.softwareReason === ""
              ? qsTr("No usable graphics card; Direct3D fell back to software.") : ""
        hintTone: "warning"

        UiText {
            Layout.alignment: Qt.AlignVCenter
            Layout.minimumWidth: Theme.fieldMd
            Layout.maximumWidth: Theme.fieldMd * 2
            text: root.rendererText()
            size: "md"
            elide: Text.ElideRight
        }
    }

    FileDialog {
        id: executableDialog
        property string toolId: ""
        title: qsTr("Choose executable")
        fileMode: FileDialog.OpenFile
        nameFilters: [qsTr("Programs (*.exe)")]
        onAccepted: Tools.setOverrideUrl(toolId, selectedFile)
    }
}
