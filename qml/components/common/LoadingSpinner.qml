import QtQuick
import SpanTui

// Loading indicator: the `loading` icon (docs/ui/icons.md) — a prompt with a
// comet of dust orbiting it. Its loop lives on the icon's hover layer, the
// one layer allowed to loop, so spinning holds `hovered`. Gated on effective
// visibility: a hidden spinner would keep the motion clock ticking. Reduced
// motion leaves the still glyph.
Item {
    id: root

    property bool active: false
    property real size: Theme.iconMd

    readonly property bool spinning: active && visible

    implicitWidth: size
    implicitHeight: size
    width: size
    height: size

    Icon {
        anchors.centerIn: parent
        name: "loading"
        size: root.size
        color: Theme.muted
        hovered: root.spinning
    }
}
