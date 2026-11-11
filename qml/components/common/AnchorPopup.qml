import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import SpanTui

// Shared anchored popup: drop panel that opens below a trigger item, clamped
// to the overlay. Positioning, toggle guard and surface come from AppPopup;
// this shell owns the heading row + separator and the list/empty-state body.
// Subclasses either drive the built-in DesktopListView (model + delegate +
// emptyText) or replace contentItem entirely.
AppPopup {
    id: root

    property int popupWidth: Theme.dialogMd
    property string heading: ""
    property string emptyText: ""
    property bool alignRight: true
    property alias model: listView.model
    property alias delegate: listView.delegate
    property alias listView: listView
    property alias glideHover: listView.glideHover
    property alias glideFillColor: listView.glideFillColor
    property alias headerActions: headerSlot.data
    readonly property bool hasHeader: heading.length > 0
                                      || headerSlot.children.length > 0

    padding: 0
    implicitWidth: popupWidth
    anchorAlign: alignRight ? "right" : "left"

    contentItem: ColumnLayout {
        spacing: 0

        RowLayout {
            visible: root.hasHeader
            Layout.fillWidth: true
            Layout.leftMargin: Theme.spaceLg
            Layout.rightMargin: Theme.spaceLg
            Layout.topMargin: Theme.spaceMd
            Layout.bottomMargin: Theme.spaceMd
            spacing: Theme.spaceMd

            UiText {
                Layout.fillWidth: true
                visible: root.heading.length > 0
                text: root.heading
                size: "md"
                elide: Text.ElideRight
            }
            RowLayout {
                id: headerSlot
                spacing: Theme.spaceSm
            }
        }

        PixelLine {
            visible: root.hasHeader
            Layout.fillWidth: true
            lineColor: Theme.separator
            opacity: Theme.fadeSm
        }

        Item {
            Layout.fillWidth: true
            Layout.fillHeight: true

            UiText {
                anchors.centerIn: parent
                visible: root.emptyText.length > 0 && listView.count === 0
                text: root.emptyText
                tone: "muted"
            }

            DesktopListView {
                id: listView
                anchors.fill: parent
                clip: true
                boundsBehavior: Flickable.StopAtBounds
                // Runs into the popup's rounded bottom corner.
                scrollBarEndInset: Theme.radiusLg
            }
        }
    }
}
