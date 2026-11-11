import QtQuick
import QtQuick.Layouts
import SpanTui

RenderText {
    // Group headers carry their own gap: SettingsSection owns no spacing, and the
    // preceding item's bottom pad already gives the group its break.
    Layout.bottomMargin: Theme.spaceMd

    color: Theme.muted
    font: {
        const f = Appearance.uiFont(Theme.uiMd, Appearance.fontRevision)
        f.letterSpacing = 1.0
        f.capitalization = Font.AllUppercase
        return f
    }
    elide: Text.ElideRight
}
