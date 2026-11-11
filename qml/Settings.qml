pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtQuick.Window
import SpanTui

// A modal sheet driven by `window.page === "settings"`: the Main.qml Loader
// opens it and popupClosed resets the page.
AppPopup {
    id: root

    property string activeSection: "general"

    // Page-level sheet size cap: the dialog ladder tops out at 640 and the
    // settings form needs more room, so the sheet keeps its own named size.
    readonly property int sheetW: 800
    readonly property int sheetH: 720
    // Form column cap inside the shared viewport.
    readonly property int contentW: 860

    anchors.centerIn: Overlay.overlay
    width: Overlay.overlay
           ? Math.min(sheetW, Overlay.overlay.width - Theme.spaceLg * 2)
           : sheetW
    height: Overlay.overlay
            ? Math.min(sheetH, Overlay.overlay.height - Theme.spaceLg * 2)
            : sheetH
    padding: Theme.spaceLg
    modal: true
    dimmed: true
    // While the scheme editor is up, Esc steps back to the scheme list
    // instead of dismissing the whole sheet.
    closeOnEscape: !colorSchemesPage.editorOpen

    Shortcut {
        sequence: "Escape"
        enabled: colorSchemesPage.editorOpen
        onActivated: colorSchemesPage.closeEditor()
    }

    // Section switches and editor takeovers reopen at the top of the sheet.
    onActiveSectionChanged: sectionView.contentItem.contentY = 0
    Connections {
        target: colorSchemesPage
        function onEditorOpenChanged() {
            sectionView.contentItem.contentY = 0
        }
    }

    contentItem: ColumnLayout {
        spacing: Theme.spaceLg

        // Header: title & dynamic subtitle + close button.
        RowLayout {
            Layout.fillWidth: true
            spacing: Theme.spaceMd

            Column {
                Layout.fillWidth: true
                spacing: Theme.spaceSm

                UiText {
                    width: parent.width
                    text: qsTr("Settings")
                    size: "lg"
                    elide: Text.ElideRight
                }

                UiText {
                    width: parent.width
                    text: colorSchemesPage.editorOpen
                          ? qsTr("Color scheme editor")
                          : root.activeSection === "colors"
                            ? qsTr("Workspace and terminal color schemes")
                            : root.activeSection === "interface"
                              ? qsTr("Typography, text clarity, and font sizing")
                              : root.activeSection === "terminal"
                                ? qsTr("Terminal display, cursor, fonts, and execution")
                                : root.activeSection === "shortcuts"
                                  ? qsTr("Keyboard shortcuts for terminal, files, and Git")
                                  : root.activeSection === "remote"
                                    ? qsTr("SSH prompts and Tailscale devices")
                                    : root.activeSection === "environment"
                                      ? qsTr("External tools SpanTui depends on")
                                      : root.activeSection === "developer"
                                        ? qsTr("Developer tools such as updates from local builds")
                                        : qsTr("Display language, appearance, and system preferences")
                    tone: "muted"
                    size: "sm"
                    elide: Text.ElideRight
                }
            }

            ChromeButton {
                kind: "ghost"
                iconName: "x"
                tip: colorSchemesPage.editorOpen ? qsTr("Back to settings")
                                                 : qsTr("Close settings")
                onClicked: {
                    if (colorSchemesPage.editorOpen)
                        colorSchemesPage.closeEditor()
                    else
                        root.close()
                }
            }
        }

        PixelLine {
            Layout.fillWidth: true
            Layout.preferredHeight: implicitHeight
            lineColor: Theme.border
            opacity: Theme.fadeMd
        }

        // Body: left nav list + vertical seam + content plane.
        RowLayout {
            Layout.fillWidth: true
            Layout.fillHeight: true
            spacing: 0

            Item {
                Layout.preferredWidth: PixelGrid.snapSize(Theme.sizeLg * 5,
                                                        root.renderDpr)
                Layout.fillHeight: true
                visible: !colorSchemesPage.editorOpen

                DesktopListView {
                    id: sectionList
                    anchors.fill: parent
                    anchors.rightMargin: Theme.spaceMd
                    clip: true
                    spacing: Theme.spaceSm
                    model: [
                        { key: "general",   icon: "sliders",   text: qsTr("General") },
                        { key: "interface", icon: "layout",    text: qsTr("Interface") },
                        { key: "terminal",  icon: "terminal",  text: qsTr("Terminal") },
                        { key: "colors",    icon: "palette",   text: qsTr("Colors") },
                        { key: "shortcuts", icon: "keyboard",  text: qsTr("Shortcuts") },
                        { key: "remote",    icon: "server",    text: qsTr("Remote") },
                        { key: "environment", icon: "cpu",     text: qsTr("Environment") },
                        { key: "developer", icon: "code",      text: qsTr("Developer") }
                    ]
                    glideHover: true
                    delegate: NavRow {
                        required property var modelData
                        width: ListView.view.width
                        icon: modelData.icon
                        label: modelData.text
                        selected: modelData.key === root.activeSection
                        onClicked: root.activeSection = modelData.key
                    }
                }
            }

            PixelLine {
                Layout.fillHeight: true
                Layout.preferredWidth: implicitWidth
                vertical: true
                lineColor: Theme.border
                visible: !colorSchemesPage.editorOpen
            }

            Item {
                Layout.fillWidth: true
                Layout.fillHeight: true
                Layout.leftMargin: colorSchemesPage.editorOpen ? 0 : Theme.spaceLg

                // The only scroll viewport; sections never scroll. It bleeds
                // through the right padding so the bar sits on the edge.
                DesktopScrollView {
                    id: sectionView
                    anchors.fill: parent
                    anchors.rightMargin: -Theme.spaceLg
                    clip: true
                    contentWidth: availableWidth
                    ScrollBar.horizontal.policy: ScrollBar.AlwaysOff

                    ColumnLayout {
                        width: Math.min(sectionView.availableWidth - Theme.spaceLg,
                                        root.contentW)
                        spacing: Theme.spaceLg

                        // Colors Section (Terminal Color Scheme Manager)
                        TerminalColorSchemeSettings {
                            id: colorSchemesPage
                            visible: root.activeSection === "colors"
                        }

                        GeneralSettingsSection {
                            visible: root.activeSection === "general"
                        }

                        InterfaceSettingsSection {
                            visible: root.activeSection === "interface"
                        }

                        TerminalSettingsSection {
                            visible: root.activeSection === "terminal"
                        }

                        ShortcutSettingsSection {
                            visible: root.activeSection === "shortcuts"
                        }

                        RemoteSettingsSection {
                            visible: root.activeSection === "remote"
                            onCloseRequested: root.close()
                        }

                        EnvironmentSettingsSection {
                            visible: root.activeSection === "environment"
                        }

                        DeveloperSettingsSection {
                            visible: root.activeSection === "developer"
                        }
                    }
                }
            }
        }
    }
}
