import QtQuick
import SpanTui

// Editor/monospace text on the pinned render path. `px` picks the size —
// prefer Theme.editorFontSize (default) or Theme.uiSm for secondary glyphs.
RenderText {
    property int px: Theme.editorFontSize
    font: Appearance.editorFont(px, Appearance.fontRevision)
}
