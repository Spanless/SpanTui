pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import SpanTui

// Terminal color scheme management: the two active slots (dark/light theme),
// the scheme list with preview strips, and the embedded editor. Built-in
// schemes are read-only; Edit/Duplicate on them opens the editor in copy
// mode, user schemes save in place. Both faces are plain section content —
// the sheet's shared viewport scrolls them.
SettingsSection {
    id: root

    readonly property real renderDpr: PixelGrid.safeDpr(Screen.devicePixelRatio)
    property var schemes: []
    property var schemeNames: []
    property bool editorOpen: false

    function refresh() {
        schemes = TerminalSettings.colorSchemes()
        schemeNames = schemes.map(scheme => scheme.name)
    }

    function closeEditor() {
        root.editorOpen = false
    }

    function openEditor(scheme, asCopy) {
        editor.sourceScheme = scheme
        editor.editingName = asCopy || scheme.builtIn ? "" : scheme.name
        editor.loadDraft()
        root.editorOpen = true
    }

    Component.onCompleted: refresh()

    Connections {
        target: TerminalSettings
        function onColorSchemesChanged() { root.refresh() }
    }

    ColumnLayout {
        id: listColumn
        Layout.fillWidth: true
        visible: !root.editorOpen
        spacing: Theme.spaceMd

        // Grid row: label 4 + combo 6 + label 4 + combo 6 + button 4 = 24.
        // Combos fill their cell — the grid, not the content, owns the width.
        GridRow {
            Layout.fillWidth: true
            gutter: Theme.spaceMd

            GridCol {
                span: 4
                UiText {
                    width: parent.width
                    anchors.verticalCenter: parent.verticalCenter
                    text: qsTr("Dark theme scheme")
                    tone: "muted"
                    size: "sm"
                    elide: Text.ElideRight
                }
            }

            GridCol {
                span: 6
                AppComboBox {
                    id: darkSlotBox
                    width: parent.width
                    anchors.verticalCenter: parent.verticalCenter
                    model: root.schemeNames
                    currentIndex: root.schemeNames.indexOf(
                                      TerminalSettings.colorSchemeDark)
                    onActivated: index => {
                        TerminalSettings.colorSchemeDark = root.schemeNames[index]
                    }
                }
            }

            GridCol {
                span: 4
                UiText {
                    width: parent.width
                    anchors.verticalCenter: parent.verticalCenter
                    text: qsTr("Light theme scheme")
                    tone: "muted"
                    size: "sm"
                    elide: Text.ElideRight
                }
            }

            GridCol {
                span: 6
                AppComboBox {
                    id: lightSlotBox
                    width: parent.width
                    anchors.verticalCenter: parent.verticalCenter
                    model: root.schemeNames
                    currentIndex: root.schemeNames.indexOf(
                                      TerminalSettings.colorSchemeLight)
                    onActivated: index => {
                        TerminalSettings.colorSchemeLight = root.schemeNames[index]
                    }
                }
            }

            GridCol {
                span: 4
                ChromeButton {
                    anchors.right: parent.right
                    anchors.verticalCenter: parent.verticalCenter
                    kind: "primary"
                    text: qsTr("Add scheme")
                    onClicked: root.openEditor(null, true)
                }
            }
        }

        PixelLine {
            Layout.fillWidth: true
            Layout.preferredHeight: implicitHeight
            lineColor: Theme.separator
            opacity: Theme.fadeMd
        }

        Repeater {
            model: root.schemes

            ColumnLayout {
                id: schemeRow
                required property int index
                required property var modelData
                Layout.fillWidth: true
                spacing: Theme.spaceMd

                RowLayout {
                    Layout.fillWidth: true
                    spacing: Theme.spaceMd

                    // Preview strip: scheme background with the 16 ANSI
                    // slots and a default-foreground sample.
                    CrispSurface {
                        devicePixelRatio: root.renderDpr
                        radius: Theme.radiusSm
                        implicitWidth: previewBody.width + Theme.spaceMd * 2
                        implicitHeight: previewBody.height + Theme.spaceMd * 2
                        fillTopColor: schemeRow.modelData.uiBackground || schemeRow.modelData.background
                        fillMiddleColor: schemeRow.modelData.background
                        fillBottomColor: schemeRow.modelData.uiAccent || schemeRow.modelData.background
                        strokeEnabled: true
                        strokeWidthPx: 1
                        strokeColor: Theme.border

                        Row {
                            id: previewBody
                            anchors.centerIn: parent
                            spacing: Theme.spaceMd

                            TerminalText {
                                text: qsTr("Aa")
                                color: schemeRow.modelData.foreground
                                anchors.verticalCenter: parent.verticalCenter
                            }

                            Grid {
                                columns: 8
                                columnSpacing: 1
                                rowSpacing: 1
                                anchors.verticalCenter: parent.verticalCenter
                                Repeater {
                                    model: schemeRow.modelData.ansi
                                    delegate: Rectangle {
                                        required property var modelData
                                        width: 8       // style-ok: dirty status dot
                                        height: 8      // style-ok: dirty status dot
                                        radius: 1
                                        color: modelData
                                    }
                                }
                            }
                        }
                    }

                    ColumnLayout {
                        spacing: Theme.spaceSm

                        UiText {
                            text: schemeRow.modelData.name
                            size: "md"
                            elide: Text.ElideRight
                            Layout.fillWidth: true
                        }

                        UiText {
                            Layout.fillWidth: true
                            elide: Text.ElideRight
                            text: {
                                const tags = []
                                if (schemeRow.modelData.builtIn)
                                    tags.push(qsTr("Built-in"))
                                if (schemeRow.modelData.name
                                        === TerminalSettings.colorSchemeDark)
                                    tags.push(qsTr("Dark theme"))
                                if (schemeRow.modelData.name
                                        === TerminalSettings.colorSchemeLight)
                                    tags.push(qsTr("Light theme"))
                                return tags.join(" · ")
                            }
                            tone: "muted"
                            size: "sm"
                        }
                    }

                    Item { Layout.fillWidth: true }

                    Row {
                        spacing: Theme.spaceSm

                        ChromeButton {
                            kind: "secondary"
                            text: qsTr("Edit")
                            onClicked: root.openEditor(schemeRow.modelData, false)
                        }

                        ChromeButton {
                            visible: !schemeRow.modelData.builtIn
                            kind: "danger"
                            text: qsTr("Delete")
                            onClicked:
                                TerminalSettings.removeColorScheme(
                                    schemeRow.modelData.name)
                        }
                    }
                }

                PixelLine {
                    Layout.fillWidth: true
                    Layout.preferredHeight: implicitHeight
                    lineColor: Theme.separator
                    opacity: Theme.fadeSm
                    visible: schemeRow.index < root.schemes.length - 1
                }
            }
        }
    }

    ColorSchemeEditor {
        id: editor
        Layout.fillWidth: true
        visible: root.editorOpen
        onFinished: root.editorOpen = false
    }
}
