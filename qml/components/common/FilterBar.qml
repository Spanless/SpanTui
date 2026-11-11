import QtQuick
import QtQuick.Layouts
import SpanTui

// Shared pane filter strip: padded UiField. Lives at the top of lists (Git
// refs, commit history); callers drive it through the exposed field or the
// forwarded signals. No bottom hairline — the sunken field alone separates
// it from the list below. The horizontal inset matches the ListItem surface
// (spaceSm), so the field's edges line up with the row highlights under it.
Item {
    id: root

    property alias field: inner
    property alias text: inner.text
    property alias placeholderText: inner.placeholderText

    signal edited(string text)
    signal accepted()

    implicitHeight: inner.implicitHeight + Theme.spaceMd * 2

    UiField {
        id: inner
        anchors.fill: parent
        anchors.leftMargin: Theme.spaceSm
        anchors.rightMargin: Theme.spaceSm
        anchors.topMargin: Theme.spaceMd
        anchors.bottomMargin: Theme.spaceMd
        size: "md"
        leadingIcon: "search"
        onTextChanged: root.edited(text)
        onAccepted: root.accepted()
    }
}
