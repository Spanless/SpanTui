import QtQuick
import SpanTui

// In-pane loading placeholder. Fills the pane so empty-state copy cannot
// show through. Does not capture window input.
Item {
    id: root

    property bool active: false
    property string message: ""

    // Pane-scope overlay tier — above pane content, below window chrome.
    readonly property int layerOverlay: 20

    visible: active
    z: layerOverlay
    implicitWidth: content.implicitWidth
    implicitHeight: content.implicitHeight

    Rectangle {
        anchors.fill: parent
        color: Theme.surfaceRaised
    }

    Column {
        id: content
        anchors.centerIn: parent
        spacing: Theme.spaceMd

        LoadingSpinner {
            anchors.horizontalCenter: parent.horizontalCenter
            active: root.active
        }

        UiText {
            visible: root.message.length > 0
            anchors.horizontalCenter: parent.horizontalCenter
            text: root.message
            tone: "muted"
            size: "md"
        }
    }
}
