import QtQuick
import SpanTui

// Shared ListView chrome for tree-style lists (file tree, project list, …).
//
// Expand/collapse inserts and removes many rows. A non-zero *remove*
// Transition keeps those delegates alive until the animation finishes; if the
// user re-expands the same folder while they are still dying, ListView queues
// the insert behind the pending removes and the chevron feels frozen for
// ~1–2s. Destroy removed rows immediately; only nudge surviving rows.
DesktopListView {
    id: root

    // Row metrics exposed to delegates through ListView.view — ListItem reads
    // them for its linked height/content inset.
    property int itemHeight: Theme.sizeMd
    property int itemPadX: Theme.spaceMd

    clip: true
    spacing: 0
    cacheBuffer: Math.max(Theme.sizeSm * 16, 320)
    boundsBehavior: Flickable.StopAtBounds
}
