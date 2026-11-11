import QtQuick

// Grid cell for GridRow's 24-column layout. `span` is the share of the
// row's columns this cell owns; `hug` instead sizes the cell to its
// content's implicitWidth and takes the width out of the span pool.
// Content inside takes the cell's geometry; the cell itself never sizes
// from its content horizontally unless hugged. Vertically the cell is
// exactly its content's implicitHeight — the row centers cells, so no
// height binding between row and cell can loop.
Item {
    id: root

    default property alias content: root.data
    property int span: 24
    // hug: take content implicitWidth instead of a span share. The row
    // subtracts hugged widths before dividing the remainder among spans.
    property bool hug: false

    // GridRow.relayout() is imperative — nudge it when this cell's share,
    // participation, or hugged width changes, otherwise the row keeps the
    // stale geometry.
    onSpanChanged: if (parent && parent.relayout)
                       parent.relayout()
    onHugChanged: if (parent && parent.relayout)
                      parent.relayout()
    onVisibleChanged: if (parent && parent.relayout)
                          parent.relayout()
    onImplicitWidthChanged: if (hug && parent && parent.relayout)
                                parent.relayout()
    onImplicitHeightChanged: if (parent && parent.relayout)
                                 parent.relayout()

    implicitWidth: {
        if (!hug)
            return 0
        var w = 0
        for (var i = 0; i < children.length; ++i) {
            var c = children[i]
            if (c && c.visible !== false)
                w = Math.max(w, c.implicitWidth || 0)
        }
        return w
    }
    implicitHeight: {
        var h = 0
        for (var i = 0; i < children.length; ++i) {
            var c = children[i]
            if (c && c.visible !== false)
                h = Math.max(h, c.implicitHeight || 0)
        }
        return h
    }

    // Cell height follows content; the row centers cells by writing y.
    // Never bind cell height to the row — that is the binding-loop class
    // this contract exists to avoid.
    height: implicitHeight
}
