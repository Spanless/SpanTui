import QtQuick
import QtQuick.Controls
import SpanTui

// Title-bar branch popup. The surface is AppPopup's own background: an
// opaque floating PopupSurface drawn behind the content. The popup must not
// clip — the surface paints its shadow and anti-aliased rim outside its own
// rectangle, so a clipped popup cut them flat and left grey wedges at the
// four rounded corners.
AnchorPanel {
    id: root
    required property GitRepository repository

    // `parent` is the overlay once the popup is opened.
    readonly property real maxHeight: Math.min(
        Theme.dialogMd, parent ? parent.height - Theme.spaceLg * 2 : Theme.dialogMd)

    width: PixelGrid.snapSize(Theme.dialogMd, renderDpr)
    // Sized to the rows, up to what the window leaves: a repository with two
    // branches gets a short popup instead of a tall empty one.
    height: PixelGrid.snapSize(Math.min(refsTree.implicitHeight, maxHeight), renderDpr)
    padding: 0
    autoSizeToContent: false

    onOpening: {
        refsTree.clearFilterAndFocus()
        Qt.callLater(function() { refsTree.clearFilterAndFocus() })
        // Uncommitted-change marks of the worktrees: checked in the
        // background, one worktree at a time; rows fill in as they land.
        if (root.repository)
            root.repository.probeWorktreeChanges()
    }

    contentItem: Item {
        GitRefsTree {
            id: refsTree
            repository: root.repository
            anchors.fill: parent
            onDismissRequested: root.close()
        }
    }
}
