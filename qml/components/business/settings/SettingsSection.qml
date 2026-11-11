import QtQuick
import QtQuick.Layouts
import SpanTui

// Shared scaffold for one settings section: the form container, a
// ColumnLayout with zero spacing — each SettingRow owns a bottom-only
// Theme.spaceLg pad that also hosts its hint line. Sections never own scrolling — the sheet's
// shared viewport in Settings.qml is the single scroll owner, so content
// here only declares sizes and fills the column width. Field-type
// controls take Theme.fieldMd (250) as their fixed width inside
// SettingRow slots — no stretching, no per-row width math.
ColumnLayout {
    id: root

    spacing: 0
    Layout.fillWidth: true
}
