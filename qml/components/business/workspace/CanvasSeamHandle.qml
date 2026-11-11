import QtQuick
import SpanTui

// Canvas seam handle: hit-only at rest (tiles own the hairline seams); the
// rail follows the shared ResizeHandle grammar — muted on hover, accent while
// dragging.
ResizeHandle {
    visualMode: ResizeHandle.GutterCenter
    restColor: "transparent"
    railVisible: true
}
