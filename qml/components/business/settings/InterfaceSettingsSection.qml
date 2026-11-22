import QtQuick
import QtQuick.Layouts
import SpanTui

// Interface section: every font setting. The font set (Appearance.fontPreset)
// picks the faces of all five groups — UI, auxiliary, hint, viewer and
// terminal; outside the custom set each group is its size row alone. Only
// the terminal group warns about a non-monospace face: only the terminal
// grid needs one.
SettingsSection {
    id: root

    readonly property bool customFonts: Appearance.fontPreset === "custom"

    SectionLabel {
        text: qsTr("Fonts")
    }

    SettingRow {
        title: qsTr("Font set")
        description: qsTr("System uses the platform's interface and monospace fonts. Span uses the bundled Span Sans and Span Mono. Custom lets you pick each font, starting from the set in use. Font sizes apply to every set.")

        SegmentedControl {
            Layout.preferredWidth: Theme.fieldMd
            Layout.alignment: Qt.AlignVCenter
            model: [
                { key: "system", text: qsTr("System") },
                { key: "span", text: "Span" },
                { key: "custom", text: qsTr("Custom") }
            ]
            current: Appearance.fontPreset
            onPicked: key => Appearance.fontPreset = key
        }
    }

    FontSettingsGroup {
        showTitle: root.customFonts
        showFaces: root.customFonts
        title: qsTr("UI Typography")
        primaryTitle: qsTr("UI primary font")
        primaryDescription: qsTr("Main typeface used for interface chrome, project trees, tab headers, and labels.")
        fallbackTitle: qsTr("UI fallback font")
        fallbackDescription: qsTr("Fallback typeface used when the primary UI font lacks glyphs (such as CJK characters).")
        sizeTitle: qsTr("UI font size")
        sizeDescription: qsTr("Base pixel size for interface text and controls.")
        weightTitle: qsTr("UI font weight")
        weightDescription: qsTr("Selects the font weight style provided by the active UI font family.")
        fontModel: Appearance.availableFonts
        primaryFamily: Appearance.uiPrimaryFontFamily
        fallbackFamily: Appearance.uiFallbackFontFamily
        fontSize: Appearance.uiFontSize
        minimumSize: 10
        maximumSize: 18
        weightModel: Appearance.availableUiFontWeights
        fontWeight: Appearance.uiFontWeight
        onPrimaryFamilyPicked: family => Appearance.uiPrimaryFontFamily = family
        onFallbackFamilyPicked: family => Appearance.uiFallbackFontFamily = family
        onFontSizeEdited: size => Appearance.uiFontSize = size
        onFontWeightPicked: weight => Appearance.uiFontWeight = weight
    }

    FontSettingsGroup {
        showTitle: root.customFonts
        showFaces: root.customFonts
        title: qsTr("Auxiliary typography")
        primaryTitle: qsTr("Auxiliary primary font")
        primaryDescription: qsTr("Typeface for small auxiliary text: badges, metadata, section labels, and secondary labels.")
        fallbackTitle: qsTr("Auxiliary fallback font")
        fallbackDescription: qsTr("Fallback typeface used when the auxiliary font lacks glyphs (such as CJK characters).")
        sizeTitle: qsTr("Auxiliary font size")
        sizeDescription: qsTr("Pixel size for small auxiliary text. Drives the sm text tier everywhere.")
        weightTitle: qsTr("Auxiliary font weight")
        weightDescription: qsTr("Selects the font weight style provided by the auxiliary font family.")
        fontModel: Appearance.availableFonts
        primaryFamily: Appearance.uiAuxPrimaryFontFamily
        fallbackFamily: Appearance.uiAuxFallbackFontFamily
        fontSize: Appearance.uiAuxFontSize
        minimumSize: 8
        maximumSize: 16
        weightModel: Appearance.availableUiAuxFontWeights
        fontWeight: Appearance.uiAuxFontWeight
        onPrimaryFamilyPicked: family => Appearance.uiAuxPrimaryFontFamily = family
        onFallbackFamilyPicked: family => Appearance.uiAuxFallbackFontFamily = family
        onFontSizeEdited: size => Appearance.uiAuxFontSize = size
        onFontWeightPicked: weight => Appearance.uiAuxFontWeight = weight
    }

    FontSettingsGroup {
        showTitle: root.customFonts
        showFaces: root.customFonts
        title: qsTr("Form hint typography")
        primaryTitle: qsTr("Hint primary font")
        primaryDescription: qsTr("Typeface for form validation and note lines shown under input controls.")
        fallbackTitle: qsTr("Hint fallback font")
        fallbackDescription: qsTr("Fallback typeface used when the hint font lacks glyphs (such as CJK characters).")
        sizeTitle: qsTr("Hint font size")
        sizeDescription: qsTr("Pixel size for form hint text under input controls. Drives the hint text tier everywhere.")
        weightTitle: qsTr("Hint font weight")
        weightDescription: qsTr("Selects the font weight style provided by the hint font family.")
        fontModel: Appearance.availableFonts
        primaryFamily: Appearance.formHintPrimaryFontFamily
        fallbackFamily: Appearance.formHintFallbackFontFamily
        fontSize: Appearance.formHintFontSize
        minimumSize: 8
        maximumSize: 16
        weightModel: Appearance.availableFormHintFontWeights
        fontWeight: Appearance.formHintFontWeight
        onPrimaryFamilyPicked: family => Appearance.formHintPrimaryFontFamily = family
        onFallbackFamilyPicked: family => Appearance.formHintFallbackFontFamily = family
        onFontSizeEdited: size => Appearance.formHintFontSize = size
        onFontWeightPicked: weight => Appearance.formHintFontWeight = weight
    }

    FontSettingsGroup {
        showTitle: root.customFonts
        showFaces: root.customFonts
        title: qsTr("Viewer typography")
        primaryTitle: qsTr("Viewer primary font")
        primaryDescription: qsTr("Fixed-pitch monospace font for file preview, line numbers, and diffs.")
        fallbackTitle: qsTr("Viewer fallback font")
        fallbackDescription: qsTr("Fallback monospace font used when the viewer primary font lacks glyphs.")
        sizeTitle: qsTr("Viewer font size")
        sizeDescription: qsTr("Pixel size for file preview, diffs, and line numbers.")
        weightTitle: qsTr("Viewer font weight")
        weightDescription: qsTr("Selects the font weight style provided by the active viewer font family.")
        fontModel: Appearance.availableFonts
        primaryFontModel: Appearance.availableFixedPitchFonts
        primaryFamily: Appearance.editorPrimaryFontFamily
        fallbackFamily: Appearance.editorFallbackFontFamily
        fontSize: Appearance.editorFontSize
        minimumSize: 10
        maximumSize: 22
        weightModel: Appearance.availableEditorFontWeights
        fontWeight: Appearance.editorFontWeight
        onPrimaryFamilyPicked: family => Appearance.editorPrimaryFontFamily = family
        onFallbackFamilyPicked: family => Appearance.editorFallbackFontFamily = family
        onFontSizeEdited: size => Appearance.editorFontSize = size
        onFontWeightPicked: weight => Appearance.editorFontWeight = weight
    }

    SettingRow {
        title: qsTr("Viewer line height")
        description: qsTr("Line height multiplier for file preview and diffs.")

        SegmentedControl {
            Layout.preferredWidth: Theme.fieldMd
            Layout.alignment: Qt.AlignVCenter
            model: [
                { key: 1.0, text: "1.0" },
                { key: 1.25, text: "1.25" },
                { key: 1.5, text: "1.5" },
                { key: 2.0, text: "2.0" }
            ]
            current: Appearance.editorLineHeight
            onPicked: key => Appearance.editorLineHeight = key
        }
    }

    FontSettingsGroup {
        showTitle: root.customFonts
        showFaces: root.customFonts
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
}
