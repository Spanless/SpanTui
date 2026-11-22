import QtQuick
import Qt.labs.platform as Platform
import SpanTui

// The system menu bar (macOS). The application menu keeps the system's
// Hide, Show All and Quit; Settings joins it through its role. Key
// equivalents follow the shortcut registry, so a rebinding moves the menu's
// key too. A focused terminal or file viewer claims the chords bound to
// its own actions before the menu sees them.
Platform.MenuBar {
    id: root

    required property AppCommands commands

    function bound(id: string): string {
        const _ = Shortcuts.revision
        return Shortcuts.sequence(id)
    }

    Platform.Menu {
        title: qsTr("File")

        Platform.MenuItem {
            text: qsTr("New Terminal")
            shortcut: root.bound("app.newTerminal")
            onTriggered: root.commands.run("app.newTerminal")
        }
        Platform.MenuItem {
            text: qsTr("Open Folder…")
            shortcut: root.bound("app.openFolder")
            onTriggered: root.commands.run("app.openFolder")
        }
        Platform.MenuSeparator {}
        Platform.MenuItem {
            text: qsTr("Close Tile")
            shortcut: root.bound("app.closeTile")
            enabled: WorkspaceLayout.focusedTileId.length > 0
            onTriggered: root.commands.run("app.closeTile")
        }
        Platform.MenuItem {
            text: qsTr("Settings…")
            role: Platform.MenuItem.PreferencesRole
            shortcut: root.bound("app.settings")
            onTriggered: root.commands.run("app.settings")
        }
    }

    Platform.Menu {
        title: qsTr("Edit")

        Platform.MenuItem {
            text: qsTr("Undo")
            shortcut: StandardKey.Undo
            onTriggered: root.commands.edit("undo")
        }
        Platform.MenuItem {
            text: qsTr("Redo")
            shortcut: StandardKey.Redo
            onTriggered: root.commands.edit("redo")
        }
        Platform.MenuSeparator {}
        Platform.MenuItem {
            text: qsTr("Cut")
            shortcut: StandardKey.Cut
            onTriggered: root.commands.edit("cut")
        }
        Platform.MenuItem {
            text: qsTr("Copy")
            shortcut: StandardKey.Copy
            onTriggered: root.commands.edit("copy")
        }
        Platform.MenuItem {
            text: qsTr("Paste")
            shortcut: StandardKey.Paste
            onTriggered: root.commands.edit("paste")
        }
        Platform.MenuItem {
            text: qsTr("Select All")
            shortcut: StandardKey.SelectAll
            onTriggered: root.commands.edit("selectAll")
        }
    }

    Platform.Menu {
        title: qsTr("View")

        // Named for what they do now, as macOS menus are; a checkable item
        // would toggle its own state and drop the binding.
        Platform.MenuItem {
            text: WorkspaceLayout.leftShown ? qsTr("Hide Project Sidebar") : qsTr("Show Project Sidebar")
            shortcut: root.bound("app.toggleSidebar")
            onTriggered: root.commands.run("app.toggleSidebar")
        }
        Platform.MenuItem {
            text: WorkspaceLayout.rightShown ? qsTr("Hide Side Panel") : qsTr("Show Side Panel")
            shortcut: root.bound("app.toggleSidePanel")
            onTriggered: root.commands.run("app.toggleSidePanel")
        }
        Platform.MenuSeparator {}
        Platform.MenuItem {
            text: root.commands.window.visibility === Window.FullScreen ? qsTr("Exit Full Screen")
                                                                        : qsTr("Enter Full Screen")
            shortcut: root.bound("app.fullScreen")
            onTriggered: root.commands.run("app.fullScreen")
        }
    }

    Platform.Menu {
        title: qsTr("Window")

        Platform.MenuItem {
            text: qsTr("Minimize")
            shortcut: "Ctrl+M"
            onTriggered: WindowChrome.minimize()
        }
        Platform.MenuItem {
            text: qsTr("Zoom")
            onTriggered: WindowChrome.toggleMaximize()
        }
    }
}
