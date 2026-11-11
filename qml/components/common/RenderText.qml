import QtQuick
import SpanTui

// Product-wide text rendering policy. Font family, fallback and metrics remain
// owned by each caller; this component changes only the glyph backend.
Text {
    renderType: Text.QtRendering
}
