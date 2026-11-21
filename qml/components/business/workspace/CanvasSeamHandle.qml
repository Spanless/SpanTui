import QtQuick
import SpanTui

// Canvas seam handle: hit-only at rest (tiles own the hairline seams); the
// rail follows the shared ResizeHandle grammar — muted on hover, accent while
// dragging.
ResizeHandle {
    visualMode: ResizeHandle.GutterCenter
    // Clear hover tone, not "transparent" (clear black): the hover fade
    // then never passes through a dark tone.
    restColor: Theme.withAlpha(hoverColor, 0)
    railVisible: true
}
