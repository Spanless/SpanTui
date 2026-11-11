import QtQuick
import SpanTui

// Floating UI crosses unrelated controls, so it cannot share a surface tone
// with whatever pane happens to sit beneath it. The floating elevation recipe
// is deliberately opaque (overlaySurface), with one uniform rim and one
// shared shadow for menus, combo lists, tooltips and dialogs.
CrispSurface {
    radius: Theme.radiusLg
    elevation: "floating"
}
