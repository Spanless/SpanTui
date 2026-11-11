pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import SpanTui

// Scheme editor: name field, workspace paper and interaction state layers,
// terminal surfaces, and the 16 ANSI slots, each opening the shared palette
// popup, plus live sample strips (UI states and terminal) rendered with the
// draft colors. Built-ins always save as a copy; user schemes save
// in place and can be renamed (the active slot follows the rename). Plain
// section content — the sheet's shared viewport scrolls it.
ColumnLayout {
    id: root

    // Map from TerminalSettings.colorScheme(name) or null for a fresh scheme.
    property var sourceScheme: null
    // Original name when editing an existing user scheme ("" otherwise).
    property string editingName: ""
    readonly property bool isNewScheme: editingName.length === 0
    readonly property real renderDpr: PixelGrid.safeDpr(Screen.devicePixelRatio)
    signal finished()

    Layout.fillWidth: true
    spacing: Theme.spaceLg

    function ansiName(index) {
        const names = [
            qsTr("Black"), qsTr("Red"), qsTr("Green"), qsTr("Yellow"),
            qsTr("Blue"), qsTr("Magenta"), qsTr("Cyan"), qsTr("White")
        ]
        return index < 8 ? names[index] : qsTr("Bright %1").arg(names[index - 8])
    }

    function loadDraft() {
        const source = root.sourceScheme
        if (source) {
            nameInput.text = root.isNewScheme
                    ? source.name + " " + qsTr("copy")
                    : source.name
            draft.foreground = source.foreground
            draft.background = source.background
            draft.cursor = source.cursor
            draft.selection = source.selection
            draft.uiBackground = source.uiBackground || source.background
            draft.uiPlate = source.uiPlate || source.uiBackground || source.background
            draft.uiAccent = source.uiAccent || Theme.accent
            draft.uiHover = source.uiHover || Theme.stateHover
            draft.uiSelection = source.uiSelection || Theme.stateSelected
            draft.uiPressed = source.uiPressed || Theme.statePressed
            draft.ansi = source.ansi.slice()
        } else {
            nameInput.text = ""
            const seed = TerminalSettings.colorScheme(TerminalSettings.colorSchemeDark)
            draft.foreground = seed.foreground
            draft.background = seed.background
            draft.cursor = seed.cursor
            draft.selection = seed.selection
            draft.uiBackground = seed.uiBackground || seed.background
            draft.uiPlate = seed.uiPlate || seed.uiBackground || seed.background
            draft.uiAccent = seed.uiAccent || Theme.accent
            draft.uiHover = seed.uiHover || Theme.stateHover
            draft.uiSelection = seed.uiSelection || Theme.stateSelected
            draft.uiPressed = seed.uiPressed || Theme.statePressed
            draft.ansi = seed.ansi.slice()
        }
    }

    function colorsMap() {
        return {
            foreground: draft.foreground,
            background: draft.background,
            cursor: draft.cursor,
            selection: draft.selection,
            uiBackground: draft.uiBackground,
            uiPlate: draft.uiPlate,
            uiAccent: draft.uiAccent,
            uiHover: draft.uiHover,
            uiSelection: draft.uiSelection,
            uiPressed: draft.uiPressed,
            ansi: draft.ansi,
        }
    }

    function save() {
        const name = nameInput.text.trim()
        if (!TerminalSettings.saveColorScheme(name, colorsMap()))
            return
        if (!root.isNewScheme && root.editingName !== name) {
            if (TerminalSettings.colorSchemeDark === root.editingName)
                TerminalSettings.colorSchemeDark = name
            if (TerminalSettings.colorSchemeLight === root.editingName)
                TerminalSettings.colorSchemeLight = name
            TerminalSettings.removeColorScheme(root.editingName)
        }
        root.finished()
    }

    Component.onCompleted: loadDraft()

    QtObject {
        id: draft
        property color foreground: "#ebe7df"
        property color background: "#131211"
        property color cursor: "#d86575"
        property color selection: "#3d93a5d8"
        property color uiBackground: "#131211"
        property color uiPlate: "#1b1a18"
        property color uiAccent: "#93a5d8"
        property color uiHover: "#1aebe7df"
        property color uiSelection: "#3d93a5d8"
        property color uiPressed: "#29ebe7df"
        property var ansi: []
    }

    ColorPickerPopup {
        id: picker

        property string currentKey: ""
        property int currentIndex: -1
        onColorEdited: color => {
            if (currentIndex >= 0) {
                const next = draft.ansi.slice()
                next[currentIndex] = color
                draft.ansi = next
            } else {
                draft[currentKey] = color
            }
        }
    }

    RowLayout {
        Layout.fillWidth: true
        spacing: Theme.spaceMd

        UiText {
            text: qsTr("Scheme name")
            tone: "muted"
            size: "sm"
        }

        UiField {
            id: nameInput
            Layout.fillWidth: true
            Layout.maximumWidth: 360 // style-ok: name field cap
            onAccepted: root.save()
        }

        Item { Layout.fillWidth: true }
    }

    ColumnLayout {
        Layout.fillWidth: true
        spacing: Theme.spaceMd

        SectionLabel { text: qsTr("Workspace") }

        Row {
            spacing: Theme.spaceMd

            Repeater {
                model: [
                    { key: "uiBackground", index: -1, label: qsTr("Window"), alpha: false },
                    { key: "uiPlate", index: -1, label: qsTr("Panels"), alpha: false },
                    { key: "uiAccent", index: -1, label: qsTr("Accent"), alpha: false },
                    { key: "uiHover", index: -1, label: qsTr("Hover"), alpha: true, onPlate: true },
                    { key: "uiSelection", index: -1, label: qsTr("Selected"), alpha: true, onPlate: true },
                    { key: "uiPressed", index: -1, label: qsTr("Pressed"), alpha: true, onPlate: true },
                ]

                SchemeSwatch {
                    required property var modelData
                    colorKey: modelData.key
                    ansiIndex: modelData.index
                    label: modelData.label
                    withAlpha: modelData.alpha
                    onPlate: modelData.onPlate === true
                }
            }
        }
    }

    ColumnLayout {
        Layout.fillWidth: true
        spacing: Theme.spaceMd

        SectionLabel { text: qsTr("Terminal") }

        Row {
            spacing: Theme.spaceMd

            Repeater {
                model: [
                    { key: "foreground", index: -1, label: qsTr("Foreground"), alpha: false },
                    { key: "background", index: -1, label: qsTr("Background"), alpha: false },
                    { key: "cursor", index: -1, label: qsTr("Cursor color"), alpha: false },
                    { key: "selection", index: -1, label: qsTr("Selection background"), alpha: true },
                ]

                SchemeSwatch {
                    required property var modelData
                    colorKey: modelData.key
                    ansiIndex: modelData.index
                    label: modelData.label
                    withAlpha: modelData.alpha
                }
            }
        }
    }

    ColumnLayout {
        Layout.fillWidth: true
        spacing: Theme.spaceMd

        SectionLabel { text: qsTr("Standard colors") }

        Row {
            spacing: Theme.spaceMd

            Repeater {
                model: [0, 1, 2, 3, 4, 5, 6, 7]

                SchemeSwatch {
                    required property int modelData
                    ansiIndex: modelData
                    label: root.ansiName(modelData)
                }
            }
        }

        SectionLabel { text: qsTr("Bright colors") }

        Row {
            spacing: Theme.spaceMd

            Repeater {
                model: [8, 9, 10, 11, 12, 13, 14, 15]

                SchemeSwatch {
                    required property int modelData
                    ansiIndex: modelData
                    label: root.ansiName(modelData)
                }
            }
        }
    }

    ColumnLayout {
        Layout.fillWidth: true
        spacing: Theme.spaceMd

        SectionLabel { text: qsTr("Preview") }

        // Interaction states on the draft panel plate, stacked exactly like
        // Theme.stateFill (selected under hover/pressed).
        CrispSurface {
            Layout.fillWidth: true
            Layout.preferredHeight: statePreview.implicitHeight + Theme.spaceMd * 2
            devicePixelRatio: root.renderDpr
            radius: Theme.radiusMd
            fillTopColor: draft.uiPlate
            fillMiddleColor: draft.uiPlate
            fillBottomColor: draft.uiPlate
            strokeEnabled: true
            strokeWidthPx: 1
            strokeColor: Theme.border

            Flow {
                id: statePreview
                anchors.fill: parent
                anchors.margins: Theme.spaceMd
                spacing: Theme.spaceSm

                Repeater {
                    model: [
                        { label: qsTr("Rest"), selected: false, layer: "" },
                        { label: qsTr("Hover"), selected: false, layer: "uiHover" },
                        { label: qsTr("Pressed"), selected: false, layer: "uiPressed" },
                        { label: qsTr("Selected"), selected: true, layer: "" },
                        { label: qsTr("Selected + hover"), selected: true, layer: "uiHover" },
                        { label: qsTr("Selected + pressed"), selected: true, layer: "uiPressed" },
                    ]

                    CrispSurface {
                        id: stateChip
                        required property var modelData
                        readonly property color base: modelData.selected
                            ? Theme.composite(draft.uiSelection, draft.uiPlate)
                            : draft.uiPlate
                        readonly property color fill: modelData.layer.length > 0
                            ? Theme.composite(draft[modelData.layer], base)
                            : base

                        width: stateLabel.implicitWidth + Theme.spaceMd * 2
                        height: Theme.sizeMd
                        devicePixelRatio: root.renderDpr
                        radius: Theme.radiusSm
                        fillTopColor: fill
                        fillMiddleColor: fill
                        fillBottomColor: fill
                        strokeEnabled: false

                        UiText {
                            id: stateLabel
                            anchors.centerIn: parent
                            text: stateChip.modelData.label
                            color: draft.foreground
                        }
                    }
                }
            }
        }

        CrispSurface {
            Layout.fillWidth: true
            Layout.preferredHeight: 96 // style-ok: live preview height
            devicePixelRatio: root.renderDpr
            radius: Theme.radiusMd
            fillTopColor: draft.background
            fillMiddleColor: draft.background
            fillBottomColor: draft.background
            strokeEnabled: true
            strokeWidthPx: 1
            strokeColor: Theme.border

            Column {
                anchors.fill: parent
                anchors.margins: Theme.spaceMd
                spacing: Theme.spaceMd

                Row {
                    spacing: Theme.spaceSm
                    TerminalText {
                        text: "user@host"
                        color: draft.foreground
                    }
                    TerminalText {
                        text: " ~/project"
                        color: draft.ansi[12] ?? draft.foreground
                    }
                }

                Row {
                    spacing: Theme.spaceSm

                    TerminalText {
                        text: "$ cargo build"
                        color: draft.ansi[2] ?? draft.foreground
                    }

                    Rectangle {
                        width: caretMetrics.width
                        height: caretMetrics.height
                        radius: 1
                        color: draft.cursor

                        TerminalText {
                            id: caretMetrics
                            text: " "
                            color: draft.cursor
                        }
                    }

                    Item {
                        width: selectedText.implicitWidth
                        height: selectedText.implicitHeight

                        Rectangle {
                            anchors.fill: parent
                            color: draft.selection
                        }

                        TerminalText {
                            id: selectedText
                            text: " selected text "
                            color: draft.foreground
                        }
                    }
                }

                Row {
                    spacing: Theme.spaceSm
                    TerminalText {
                        text: "error:"
                        color: draft.ansi[1] ?? draft.foreground
                    }
                    TerminalText {
                        text: "warning:"
                        color: draft.ansi[3] ?? draft.foreground
                    }
                    TerminalText {
                        text: "ok"
                        color: draft.ansi[10] ?? draft.foreground
                    }
                }
            }
        }
    }

    Row {
        Layout.fillWidth: true
        Layout.alignment: Qt.AlignRight
        spacing: Theme.spaceMd

        ChromeButton {
            kind: "secondary"
            text: qsTr("Cancel")
            onClicked: root.finished()
        }

        ChromeButton {
            kind: "primary"
            text: root.isNewScheme ? qsTr("Create scheme") : qsTr("Save")
            enabled: nameInput.text.trim().length > 0
            onClicked: root.save()
        }
    }

    // One swatch = framed color box + label; clicking opens the palette popup.
    component SchemeSwatch: Item {
        id: swatch

        property string colorKey: ""
        property int ansiIndex: -1
        property string label: ""
        property bool withAlpha: false
        // State layers are translucent; show them as they land on the panels.
        property bool onPlate: false
        readonly property color value: ansiIndex >= 0
                                      ? (draft.ansi[ansiIndex] ?? "#000000")
                                      : draft[colorKey]
        readonly property color shown: onPlate ? Theme.composite(value, draft.uiPlate)
                                               : value
        readonly property bool pressed: swatchMouse.pressed
        readonly property bool raised: swatchMouse.containsMouse && !pressed

        width: 64   // style-ok: preview swatch cell
        height: 52  // style-ok: preview swatch cell

        CrispSurface {
            anchors.top: parent.top
            anchors.horizontalCenter: parent.horizontalCenter
            width: 48     // style-ok: ANSI swatch cell
            height: 28    // style-ok: ANSI swatch cell
            radius: Theme.radiusXs
            devicePixelRatio: root.renderDpr
            fillTopColor: swatch.shown
            fillMiddleColor: swatch.shown
            fillBottomColor: swatch.shown
            strokeEnabled: true
            strokeWidthPx: 1
            // Neutral rim that darkens with the pointer; accent strokes belong
            // to keyboard focus only.
            strokeColor: swatch.pressed ? Theme.text
                         : swatch.raised ? Theme.muted : Theme.border
            shadowEnabled: false
        }

        UiText {
            anchors.bottom: parent.bottom
            anchors.horizontalCenter: parent.horizontalCenter
            width: parent.width
            text: swatch.label
            tone: "muted"
            size: "sm"
            horizontalAlignment: Text.AlignHCenter
            elide: Text.ElideRight
        }

        MouseArea {
            id: swatchMouse
            anchors.fill: parent
            hoverEnabled: true
            cursorShape: Qt.PointingHandCursor
            onClicked: {
                picker.currentKey = swatch.colorKey
                picker.currentIndex = swatch.ansiIndex
                picker.openFor(swatch, swatch.label, swatch.value, swatch.withAlpha)
            }
        }
    }
}
