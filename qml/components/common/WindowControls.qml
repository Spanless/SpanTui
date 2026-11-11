import QtQuick
import QtQuick.Layouts
import SpanTui

// Caption actions live in the frosted title bar, not on the window rim.
// Compact ChromeButton squares match the left tool cluster.
// All three stay Qt-interactive (HTCLIENT) — no HTMAXBUTTON, so Win11 Snap
// Layouts flyout does not steal maximize.
// Uses the WindowChrome singleton: only the workspace window (Main.qml),
// which provides it, shows these controls.
Row {
    id: root
    readonly property real renderDpr: PixelGrid.safeDpr(Screen.devicePixelRatio)
    height: Theme.sizeMd
    spacing: PixelGrid.snapSize(2, renderDpr)

    readonly property bool hovered: minimizeButton.hovered
                                    || maximizeButton.hovered
                                    || closeButton.hovered

    ChromeButton {
        id: minimizeButton
        size: "md"
        kind: "ghost"
        iconName: "chrome-minimize"
        onClicked: WindowChrome.minimize()
    }

    ChromeButton {
        id: maximizeButton
        size: "md"
        kind: "ghost"
        iconName: WindowChrome.maximized ? "chrome-restore" : "chrome-maximize"
        onClicked: WindowChrome.toggleMaximize()
    }

    ChromeButton {
        id: closeButton
        size: "md"
        kind: "close"
        iconName: "x"
        onClicked: WindowChrome.requestClose()
    }

    Component.onCompleted: {
        WindowChrome.addInteractiveItem(minimizeButton)
        WindowChrome.addInteractiveItem(maximizeButton)
        WindowChrome.addInteractiveItem(closeButton)
    }
}
