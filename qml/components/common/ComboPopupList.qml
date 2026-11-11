pragma Translator: "AppComboBox"

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import SpanTui

// Popup body of AppComboBox: the filtered option list (glide hover, at most
// `combo.maximumVisibleRows` rows before scrolling) or a "No matches" row.
ColumnLayout {
    id: panel

    // The owning AppComboBox: model, delegate and highlight come from it.
    required property Item combo
    readonly property int rowCount: list.count

    spacing: 0

    DesktopListView {
        id: list
        visible: list.count > 0
        Layout.fillWidth: true
        Layout.preferredHeight: Math.min(list.count, panel.combo.maximumVisibleRows)
                                * Theme.sizeMd
        clip: true
        model: panel.combo.filteredModel
        delegate: panel.combo.delegate
        currentIndex: panel.combo.highlightedIndex
        boundsBehavior: Flickable.StopAtBounds
        snapMode: ListView.SnapToItem
        highlightMoveDuration: 0
        // Flush against the popup's rounded right edge.
        scrollBarEndInset: Theme.radiusLg
        glideHover: true
        glideInsetX: 0
        glideInsetY: 0
        glideFillColor: Theme.accent
    }

    UiText {
        visible: list.count === 0
        Layout.fillWidth: true
        // The popup's right inset lives inside this panel (see listWidth).
        Layout.rightMargin: Theme.spaceSm
        Layout.preferredHeight: Theme.sizeMd
        horizontalAlignment: Text.AlignHCenter
        verticalAlignment: Text.AlignVCenter
        text: qsTr("No matches")
        tone: "muted"
        size: "sm"
    }
}
