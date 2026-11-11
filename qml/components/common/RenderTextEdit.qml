import QtQuick
import SpanTui

// Editable counterpart of RenderText. Native rendering rasterizes at the exact
// target size, avoiding the softened small-glyph edges of curve rendering.
TextEdit {
    renderType: Text.QtRendering
}
