import QtQuick
import QtQuick.Layouts
import QtQuick.Window
import SpanTui

// General section: appearance & window plus application updates.
SettingsSection {
    id: root

    // Group: Appearance & Window
    SectionLabel {
        text: qsTr("Appearance & Window")
    }

    SettingRow {
        title: qsTr("Language")
        description: qsTr("Choose the display language. Changes apply immediately to all interface components.")

        // Each language is named in itself, not translated, so it can be
        // found whatever the current language is.
        AppComboBox {
            id: languageBox

            readonly property var languages: [
                { key: "en", name: "English" },
                { key: "zh_CN", name: "简体中文" }
            ]

            Layout.preferredWidth: Theme.fieldMd
            Layout.alignment: Qt.AlignVCenter
            filterable: false
            model: languages
            itemTextForIndex: (index, modelData) => modelData.name
            currentIndex: languages.findIndex(language => language.key === Appearance.language)
            onActivated: index => Translator.setLanguage(languageBox.languages[index].key)
        }
    }

    SettingRow {
        title: qsTr("Theme")
        description: qsTr("Use the operating system color mode or choose a fixed light or dark theme.")

        SegmentedControl {
            Layout.preferredWidth: Theme.fieldMd
            Layout.alignment: Qt.AlignVCenter
            model: [
                { key: "system", text: qsTr("System") },
                { key: "light", text: qsTr("Light") },
                { key: "dark", text: qsTr("Dark") }
            ]
            current: Appearance.themeMode
            onPicked: key => Appearance.themeMode = key
        }
    }

    SettingRow {
        title: qsTr("Performance monitor refresh")
        description: qsTr("Sampling interval in seconds for the built-in CPU, GPU, and memory monitor.")

        Stepper {
            Layout.preferredWidth: Theme.fieldMd
            Layout.alignment: Qt.AlignVCenter
            value: WorkspaceSettings.performanceMonitorIntervalSeconds
            minimumValue: 1
            maximumValue: 60
            onValueEdited: v => WorkspaceSettings.performanceMonitorIntervalSeconds = v
        }
    }

    // Group: Application Updates
    SectionLabel {
        text: qsTr("Application Update")
    }

    // Local updates (Developer settings) replace the GitHub channel
    // entirely, so the section says which source is in use.
    SettingRow {
        title: qsTr("Update source")
        description: qsTr("SpanTui updates from the latest stable release published on GitHub. Turning on local updates in Developer settings replaces it with a local build folder.")
        hint: AppUpdate.localSourceEnabled
              ? qsTr("Local updates are on in Developer settings")
              : ""
        hintTone: "warning"

        FadeText {
            Layout.preferredWidth: Theme.fieldMd
            Layout.alignment: Qt.AlignVCenter
            text: !AppUpdate.localSourceEnabled
                  ? qsTr("GitHub stable releases (%1)").arg(AppUpdate.releaseRepository)
                  : AppUpdate.localSourcePath.length > 0
                    ? AppUpdate.localSourcePath
                    : qsTr("No local build folder chosen")
            tone: AppUpdate.localSourceEnabled ? "default" : "muted"
            fadeColor: Theme.overlaySurface
        }
    }

    // Only an installed copy runs the installer (UpdateService.canInstall).
    SettingRow {
        visible: AppUpdate.canInstall
        title: qsTr("Apply updates automatically")
        description: qsTr("When enabled, SpanTui installs a newer version automatically when it finds one on startup or after the local build folder changes.")

        AppSwitch {
            Layout.alignment: Qt.AlignVCenter
            checked: AppUpdate.autoApply
            onToggled: newChecked => AppUpdate.autoApply = newChecked
        }
    }

    SettingRow {
        title: qsTr("Check for updates")
        description: AppUpdate.statusMessage.length > 0
                     ? AppUpdate.statusMessage
                     : qsTr("Compare this installation with the configured update source.")

        // One stateful button: idle "Check" → busy "Checking…" → when the
        // check finds a newer version it flips to "Update & restart"
        // (primary; "Download" for a copy that cannot install) until the
        // update is applied or the source changes. While the update installs
        // it reopens the progress card.
        ChromeButton {
            Layout.alignment: Qt.AlignVCenter
            kind: AppUpdate.updateAvailable ? "primary" : "secondary"
            text: AppUpdate.applying ? qsTr("Show progress")
                  : AppUpdate.busy ? qsTr("Checking…")
                  : !AppUpdate.updateAvailable ? qsTr("Check")
                  : AppUpdate.canInstall ? qsTr("Update & restart")
                                         : qsTr("Download")
            enabled: AppUpdate.applying
                     || (!AppUpdate.busy && (AppUpdate.updateAvailable || AppUpdate.canCheck))
            onClicked: {
                if (AppUpdate.applying) {
                    if (Window.window && Window.window.requestUpdate)
                        Window.window.requestUpdate()
                } else if (AppUpdate.updateAvailable) {
                    if (Window.window && Window.window.confirmUpdate)
                        Window.window.confirmUpdate()
                } else {
                    AppUpdate.checkNow(false)
                }
            }
        }
    }
}
