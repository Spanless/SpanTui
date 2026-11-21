import QtQuick
import SpanTui

// The project-sidebar and side-panel toggles, at the leading end of the
// title band (Main.qml): after the system traffic lights on macOS, at the
// window's left edge elsewhere. A side that floats (narrow window) slides
// in and out as an overlay instead of docking. They stay flat; a shown
// side lights its region of the icon, like the macOS sidebar buttons.
Row {
    id: root

    // ChromeButton size: "md" (the toolbar size, as tall as the traffic-light
    // band allows) unless the band is lower than that.
    property string size: "md"

    spacing: Theme.spaceSm

    ChromeButton {
        anchors.verticalCenter: parent.verticalCenter
        size: root.size
        kind: "ghost"
        checked: WorkspaceLayout.leftShown
        iconName: "panel-left"
        tip: qsTr("Toggle project sidebar")
        onClicked: WorkspaceController.toggleSidePanel("left")
    }
    ChromeButton {
        anchors.verticalCenter: parent.verticalCenter
        size: root.size
        kind: "ghost"
        checked: WorkspaceLayout.rightShown
        iconName: "panel-right"
        tip: qsTr("Toggle side panel")
        onClicked: WorkspaceController.toggleSidePanel("right")
    }
}
