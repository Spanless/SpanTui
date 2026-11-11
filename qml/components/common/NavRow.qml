import QtQuick
import QtQuick.Layouts
import SpanTui

// Shared icon+label list row for nav and catalog lists (settings sections,
// sidebar sessions). Selection/hover chrome lives in ListItem; callers only
// supply the icon, label and content color — never rebuild the row surface.
// Same text rule as every list row: the label is body text (contentColor, a
// status tone when the caller needs one); the leading icon rests muted and
// takes the content color on the selected or hovered row. Selection itself is
// the row fill, never a label dimming.
ListItem {
    id: root

    property string icon: ""
    property string label: ""
    property color contentColor: Theme.text
    readonly property color iconColor: selected || hovered ? contentColor : Theme.muted
    property bool shimmer: false
    // Optional trailing content (badges, status dots) docked at the row's end.
    property alias trailing: trailingHost.data

    cursorShape: Qt.PointingHandCursor

    RowLayout {
        anchors.fill: parent
        spacing: Theme.spaceSm
        Icon {
            name: root.icon
            size: root.contentIcon
            color: root.iconColor
            hovered: root.hovered
            pressed: root.mouseArea.pressed
            active: root.selected
        }
        UiText {
            Layout.fillWidth: true
            color: root.contentColor
            shimmer: root.shimmer
            text: root.label
            elide: Text.ElideRight
        }
        Item {
            id: trailingHost
            Layout.alignment: Qt.AlignVCenter
            Layout.preferredWidth: childrenRect.width
            Layout.preferredHeight: childrenRect.height
        }
    }
}
