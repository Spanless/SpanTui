import QtQuick
import SpanTui

// The window's own commands, behind one entry: the system menu bar
// (MacMenuBar, macOS) and the window shortcuts (AppShortcuts, elsewhere)
// both call run() with a ShortcutRegistry "app.*" id, and the Edit menu
// reaches the focused item through edit().
QtObject {
    id: root

    // The ApplicationWindow (Main.qml): its page, folder dialog and focus.
    required property var window

    readonly property var ids: ["app.newTerminal", "app.openFolder", "app.closeTile", "app.settings",
                                "app.toggleSidebar", "app.toggleSidePanel", "app.fullScreen"]

    function run(id: string): void {
        switch (id) {
        case "app.newTerminal":
            WorkspaceController.createSessionInBoundScope()
            break
        case "app.openFolder":
            root.window.projectFolderDialog.open()
            break
        case "app.closeTile":
            if (WorkspaceLayout.focusedTileId.length > 0)
                WorkspaceController.requestCloseTile(WorkspaceLayout.focusedTileId)
            break
        case "app.settings":
            root.window.page = "settings"
            break
        case "app.toggleSidebar":
            WorkspaceController.toggleSidePanel("left")
            break
        case "app.toggleSidePanel":
            WorkspaceController.toggleSidePanel("right")
            break
        case "app.fullScreen":
            if (root.window.visibility === Window.FullScreen)
                root.window.showNormal()
            else
                root.window.showFullScreen()
            break
        }
    }

    // Clipboard and selection commands for whatever holds focus, or the
    // nearest ancestor that has them: the terminal's own methods first,
    // else a text control's. False when nothing takes the command.
    function edit(command: string): bool {
        const methods = {
            "copy": ["copySelection", "copy"],
            "cut": ["cut"],
            "paste": ["pasteClipboard", "paste"],
            "selectAll": ["selectAll"],
            "undo": ["undo"],
            "redo": ["redo"]
        }[command] || []
        for (let item = root.window.activeFocusItem; item; item = item.parent) {
            for (const name of methods) {
                if (typeof item[name] === "function") {
                    item[name]()
                    return true
                }
            }
        }
        return false
    }
}
