import QtQuick
import QtQuick.Dialogs
import QtQuick.Layouts
import SpanTui

// Developer section: local updates from the folder that holds the version
// folders `dev package` writes. While they are on, the update check only
// looks at the newest package there and never at GitHub releases.
SettingsSection {
    id: root

    SectionLabel {
        text: qsTr("Local Update")
    }

    SettingRow {
        title: qsTr("Update from local builds")
        description: qsTr("When enabled, SpanTui only looks for new versions in the local build folder below and ignores GitHub releases.")

        AppSwitch {
            Layout.alignment: Qt.AlignVCenter
            checked: AppUpdate.localSourceEnabled
            onToggled: newChecked => AppUpdate.localSourceEnabled = newChecked
        }
    }

    SettingRow {
        title: qsTr("Local build folder")
        description: qsTr("The folder that holds the version folders of dev.bat package, such as target\\release or span-tui under the artifact root. SpanTui takes the newest version in it and, when it is newer, runs its installer.")
        hint: AppUpdate.localSourceEnabled && AppUpdate.localSourcePath.length === 0
              ? qsTr("Choose a folder to enable local updates")
              : ""
        hintTone: "warning"

        UiField {
            id: localSourceField
            Layout.preferredWidth: Theme.fieldMd
            Layout.alignment: Qt.AlignVCenter
            text: AppUpdate.localSourcePath
            role: "mono"
            placeholderText: qsTr("C:\\…\\target\\release")
            fadeOverflow: true
            onEditingFinished: AppUpdate.localSourcePath = text.trim()

            trailing: ChromeButton {
                kind: "ghost"
                size: "sm"
                iconName: "folder"
                tip: qsTr("Browse for local build folder")
                onClicked: localSourceDialog.open()
            }
        }
    }

    FolderDialog {
        id: localSourceDialog
        title: qsTr("Choose local build folder")
        onAccepted: {
            AppUpdate.setLocalSourceFolderUrl(selectedFolder)
            localSourceField.text = AppUpdate.localSourcePath
        }
    }
}
