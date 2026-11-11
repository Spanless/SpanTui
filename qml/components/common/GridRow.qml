import QtQuick
import SpanTui

// 24-column grid row — the framework owns column geometry; children land in
// GridCol cells. A cell's width is `span / columns` of the width left after
// gutters and hugged cells; a `hug` cell instead takes its content's
// implicitWidth. Content inside a GridCol takes whatever the cell leaves;
// labels and controls never negotiate width with each other.
// Named GridRow/GridCol rather than Row/Col — a `Row` type here would
// shadow QtQuick's positioner everywhere `import SpanTui` appears.
//
// v1 contract: only GridCol children, no wrap — a row whose cells overflow
// the width clips (design the spans to fit). Cells keep their own implicit
// height; the row centers each cell vertically, so no height binding runs
// between row and cell (that loop class is banned by design here).
// Widths snap to whole pixels.
Item {
    id: root

    default property alias content: root.data
    property int gutter: Theme.spaceMd
    property int columns: 24

    implicitHeight: {
        var h = 0
        for (var i = 0; i < children.length; ++i) {
            var c = children[i]
            if (c && c.visible !== false)
                h = Math.max(h, c.implicitHeight || 0)
        }
        return h
    }

    function relayout() {
        var cells = []
        var hugTotal = 0
        for (var i = 0; i < children.length; ++i) {
            var c = children[i]
            if (c && c.visible !== false) {
                cells.push(c)
                if (c.hug)
                    hugTotal += c.implicitWidth
            }
        }
        if (cells.length === 0)
            return
        var avail = Math.max(0, width - (cells.length - 1) * gutter - hugTotal)
        var x = 0
        for (var j = 0; j < cells.length; ++j) {
            var cell = cells[j]
            var w = cell.hug ? cell.implicitWidth
                             : Math.max(0, Math.round(
                                    avail * (cell.span !== undefined
                                             ? cell.span : columns)
                                    / columns))
            cell.x = Math.round(x)
            cell.y = Math.max(0, Math.round((height - cell.implicitHeight) / 2))
            cell.width = w
            x = cell.x + w + gutter
        }
    }

    onWidthChanged: relayout()
    onHeightChanged: relayout()
    onChildrenChanged: relayout()
    Component.onCompleted: relayout()
}
