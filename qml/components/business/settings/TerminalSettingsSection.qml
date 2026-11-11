import QtQuick
import QtQuick.Controls
import QtQuick.Dialogs
import QtQuick.Layouts
import SpanTui

// Terminal section: shells, terminal typography and display & execution.
SettingsSection {
    id: root

    // Group: Shells — what a new local terminal starts.
    SectionLabel {
        text: qsTr("Shells")
    }

    SettingRow {
        title: qsTr("Default shell")
        description: qsTr("Started by \"New terminal\". A terminal opened with a specific shell from a project's menu keeps that shell.")
        hint: Shells.launchProfiles.length === 0 ? qsTr("No shell was found. Add one below.") : ""
        hintTone: "warning"

        AppComboBox {
            Layout.preferredWidth: Theme.fieldMd
            Layout.alignment: Qt.AlignVCenter
            // A short list of programs: a picker with their icons, not a filter.
            filterable: false
            model: Shells.launchProfiles
            itemTextForIndex: (index, modelData) => modelData.name
            itemProgramIconForIndex: (index, modelData) => modelData.icon
            currentIndex: model.findIndex(profile => profile.profileId === Shells.defaultProfileId)
            leadingProgramIcon: currentIndex >= 0 ? model[currentIndex].icon : ""
            onActivated: index => Shells.setDefaultProfile(model[index].profileId)
        }
    }

    Repeater {
        model: Shells

        delegate: SettingRow {
            id: shellRow
            required property string profileId
            required property string name
            required property string executable
            required property string arguments
            required property string defaultArguments
            required property bool custom
            required property bool available

            title: name
            description: qsTr("Arguments passed to the program when a terminal starts.")
            // The program path stays visible: two shells may share a name.
            hint: available ? executable
                            : qsTr("Not found: %1. Remove this shell or install the program again.").arg(executable)
            hintTone: available ? "muted" : "warning"

            UiField {
                Layout.preferredWidth: Theme.fieldMd
                Layout.alignment: Qt.AlignVCenter
                role: "mono"
                fadeOverflow: true
                text: shellRow.arguments
                placeholderText: qsTr("No arguments")
                onEditingFinished: {
                    if (text.trim() !== shellRow.arguments)
                        Shells.setArguments(shellRow.profileId, text)
                }

                trailing: Row {
                    spacing: Theme.spaceSm

                    ChromeButton {
                        kind: "ghost"
                        size: "sm"
                        iconName: "undo"
                        visible: !shellRow.custom && shellRow.arguments !== shellRow.defaultArguments
                        tip: qsTr("Restore default arguments")
                        onClicked: Shells.setArguments(shellRow.profileId, shellRow.defaultArguments)
                    }

                    ChromeButton {
                        kind: "ghost"
                        size: "sm"
                        iconName: "trash"
                        visible: shellRow.custom
                        tip: qsTr("Remove shell")
                        onClicked: Shells.removeCustomProfile(shellRow.profileId)
                    }
                }
            }
        }
    }

    SettingRow {
        title: qsTr("Add shell")
        description: qsTr("Any console program can be a shell, such as nu.exe or a specific bash.exe. Installed shells are detected automatically.")

        RowLayout {
            Layout.alignment: Qt.AlignVCenter
            spacing: Theme.spaceSm

            ChromeButton {
                kind: "ghost"
                iconName: "refresh"
                tip: qsTr("Detect installed shells again")
                onClicked: Shells.refresh()
            }

            ChromeButton {
                kind: "secondary"
                iconName: "plus"
                text: qsTr("Choose program…")
                onClicked: shellDialog.open()
            }
        }
    }

    FileDialog {
        id: shellDialog
        title: qsTr("Choose shell program")
        fileMode: FileDialog.OpenFile
        nameFilters: [qsTr("Programs (*.exe)")]
        onAccepted: Shells.addCustomProfileUrl(selectedFile)
    }

    FontSettingsGroup {
        title: qsTr("Terminal Typography")
        primaryTitle: qsTr("Terminal primary font")
        primaryDescription: qsTr("Fixed-pitch typeface used for the terminal grid and console output.")
        fallbackTitle: qsTr("Terminal fallback font")
        fallbackDescription: qsTr("Fallback typeface used when the primary terminal font lacks glyphs.")
        sizeTitle: qsTr("Terminal font size")
        sizeDescription: qsTr("Pixel size for terminal cells, command input, and shell text.")
        weightTitle: qsTr("Terminal font weight")
        weightDescription: qsTr("Selects the font weight style provided by the active terminal font family.")
        fontModel: Appearance.availableFonts
        primaryFamily: Appearance.terminalPrimaryFontFamily
        fallbackFamily: Appearance.terminalFallbackFontFamily
        fontSize: Appearance.terminalFontSize
        minimumSize: 10
        maximumSize: 22
        weightModel: Appearance.availableTerminalFontWeights
        fontWeight: Appearance.terminalFontWeight
        fixedPitchCheck: family => Appearance.isFixedPitch(family)
        onPrimaryFamilyPicked: family => Appearance.terminalPrimaryFontFamily = family
        onFallbackFamilyPicked: family => Appearance.terminalFallbackFontFamily = family
        onFontSizeEdited: size => Appearance.terminalFontSize = size
        onFontWeightPicked: weight => Appearance.terminalFontWeight = weight
    }

    // Group: Terminal Display & Execution
    SectionLabel {
        text: qsTr("Terminal Display & Execution")
    }

    SettingRow {
        title: qsTr("Cursor style")
        description: qsTr("Selects the cursor shape rendered in terminal sessions.")

        SegmentedControl {
            Layout.preferredWidth: Theme.fieldMd
            Layout.alignment: Qt.AlignVCenter
            model: [
                { key: "block", text: qsTr("Block") },
                { key: "bar", text: qsTr("Bar") },
                { key: "underline", text: qsTr("Underline") }
            ]
            current: TerminalSettings.cursorStyle
            onPicked: key => TerminalSettings.cursorStyle = key
        }
    }

    SettingRow {
        title: qsTr("Refresh rate")
        description: qsTr("Maximum frame rendering rate for terminal surfaces. Lower rates conserve CPU and power.")

        AppComboBox {
            Layout.preferredWidth: Theme.fieldMd
            Layout.alignment: Qt.AlignVCenter
            editable: true
            model: [30, 60, 120, 144, 165, 240, 360]
            itemTextForIndex: (index, modelData) => qsTr("%1 Hz").arg(modelData)
            suffixText: "Hz"
            currentValue: TerminalSettings.refreshRate
            // Face shows the bare number; the unit lives in the suffix slot.
            valueText: value => String(value)
            currentIndex: model.indexOf(TerminalSettings.refreshRate)
            parseInput: text => {
                const n = parseInt(text, 10)
                return isNaN(n) ? undefined : Math.max(30, Math.min(360, n))
            }
            onActivated: index => TerminalSettings.refreshRate = model[index]
            onSubmitted: value => TerminalSettings.refreshRate = value
        }
    }

    SettingRow {
        title: qsTr("Script launch mode")
        description: qsTr("Controls whether script files executed from the file tree open as an in-workspace project terminal session or a separate independent window.")

        SegmentedControl {
            Layout.preferredWidth: Theme.fieldMd
            Layout.alignment: Qt.AlignVCenter
            model: [
                { key: "projectSession", text: qsTr("Project session") },
                { key: "independentWindow", text: qsTr("Independent window") }
            ]
            current: WorkspaceSettings.scriptRunMode
            onPicked: key => WorkspaceSettings.scriptRunMode = key
        }
    }
}
