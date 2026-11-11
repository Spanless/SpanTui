import QtQuick
import QtQuick.Layouts
import SpanTui

// One typography group of a settings page: SectionLabel, then four
// SettingRows — primary font, fallback font, size, weight. Values come in
// through properties and edits leave as request signals; the caller owns
// the state and writes it back. Zero spacing like SettingsSection, so the
// group drops into a section column without changing its rhythm.
ColumnLayout {
    id: root

    property string title: ""
    property string primaryTitle: ""
    property string primaryDescription: ""
    property string fallbackTitle: ""
    property string fallbackDescription: ""
    property string sizeTitle: ""
    property string sizeDescription: ""
    property string weightTitle: ""
    property string weightDescription: ""

    // Family names offered by both pickers; primaryFontModel overrides the
    // primary picker alone (e.g. a fixed-pitch-only list).
    property var fontModel: []
    property var primaryFontModel: fontModel
    property string primaryFamily: ""
    property string fallbackFamily: ""

    property int fontSize: 0
    property int minimumSize: 0
    property int maximumSize: 100

    // FontWeightPicker options ({ style, weight }) for the active family.
    property var weightModel: []
    property int fontWeight: 400

    // Optional family => bool predicate. When set, a family it rejects
    // puts the warning stroke on its picker and the hint under its row.
    property var fixedPitchCheck: null

    signal primaryFamilyPicked(string family)
    signal fallbackFamilyPicked(string family)
    signal fontSizeEdited(int size)
    signal fontWeightPicked(int weight)

    spacing: 0
    Layout.fillWidth: true

    function nonFixedPitch(family) {
        return root.fixedPitchCheck !== null && !root.fixedPitchCheck(family)
    }

    SectionLabel {
        text: root.title
    }

    SettingRow {
        title: root.primaryTitle
        description: root.primaryDescription
        hint: root.nonFixedPitch(root.primaryFamily) ? qsTr("Non-monospace font") : ""
        hintTone: "warning"

        FontPicker {
            Layout.preferredWidth: Theme.fieldMd
            Layout.alignment: Qt.AlignVCenter
            model: root.primaryFontModel
            selectedFamily: root.primaryFamily
            warning: root.nonFixedPitch(root.primaryFamily)
            onFamilySelected: family => root.primaryFamilyPicked(family)
        }
    }

    SettingRow {
        title: root.fallbackTitle
        description: root.fallbackDescription
        hint: root.nonFixedPitch(root.fallbackFamily) ? qsTr("Non-monospace font") : ""
        hintTone: "warning"

        FontPicker {
            Layout.preferredWidth: Theme.fieldMd
            Layout.alignment: Qt.AlignVCenter
            model: root.fontModel
            selectedFamily: root.fallbackFamily
            warning: root.nonFixedPitch(root.fallbackFamily)
            onFamilySelected: family => root.fallbackFamilyPicked(family)
        }
    }

    SettingRow {
        title: root.sizeTitle
        description: root.sizeDescription

        Stepper {
            Layout.preferredWidth: Theme.fieldMd
            Layout.alignment: Qt.AlignVCenter
            value: root.fontSize
            minimumValue: root.minimumSize
            maximumValue: root.maximumSize
            onValueEdited: v => root.fontSizeEdited(v)
        }
    }

    SettingRow {
        title: root.weightTitle
        description: root.weightDescription

        FontWeightPicker {
            Layout.preferredWidth: Theme.fieldMd
            Layout.alignment: Qt.AlignVCenter
            model: root.weightModel
            selectedWeight: root.fontWeight
            onWeightSelected: weight => root.fontWeightPicked(weight)
        }
    }
}
