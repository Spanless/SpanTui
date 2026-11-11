import QtQuick
import SpanTui

// Hit box and visual rail stay separate. The rail is a thin light seam at rest,
// thickens + darkens on hover and turns accent only while dragging — the one
// seam state grammar for panel and canvas handles alike. The mouse area
// stays Theme.handleW.
Item {
    id: root

    enum VisualMode {
        GutterCenter,
        InnerSeam
    }

    signal dragStarted()
    signal dragged(real delta)
    signal dragEnded()

    // Qt.Vertical: vertical bar, resize width. Qt.Horizontal: row seam.
    property int orientation: Qt.Vertical
    property int visualMode: ResizeHandle.GutterCenter
    property string edge: "right"
    property bool interactionAnimationsEnabled: true
    property bool railVisible: true
    property real railEndInset: 0
    property color restColor: Theme.separator
    property color hoverColor: Theme.muted
    property color dragColor: Theme.accent

    readonly property bool verticalBar: orientation === Qt.Vertical
    implicitWidth: verticalBar ? Theme.handleW : 1
    implicitHeight: verticalBar ? 1 : Theme.handleW

    readonly property real devicePixelRatio: PixelGrid.safeDpr(Screen.devicePixelRatio)
    readonly property real hairline: PixelGrid.hairline(devicePixelRatio)
    readonly property bool engaged: dragArea.pressed || dragArea.containsMouse
    readonly property bool dragging: dragArea.pressed
    readonly property real railThickness: {
        const px = root.engaged ? Theme.handleHoverPx : 1
        return PixelGrid.snapSize(px / root.devicePixelRatio, root.devicePixelRatio)
    }
    readonly property real railAlong: PixelGrid.snap(
        ((verticalBar ? width : height) - railThickness) * 0.5,
        devicePixelRatio)

    Rectangle {
        visible: root.railVisible
        color: root.dragging ? root.dragColor
               : root.engaged ? root.hoverColor : root.restColor
        antialiasing: false
        x: root.verticalBar ? root.railAlong : root.railEndInset
        y: root.verticalBar ? root.railEndInset : root.railAlong
        width: root.verticalBar ? root.railThickness
                                : Math.max(0, parent.width - root.railEndInset * 2)
        height: root.verticalBar ? Math.max(0, parent.height - root.railEndInset * 2)
                                 : root.railThickness
    }

    MouseArea {
        id: dragArea
        anchors.fill: parent
        hoverEnabled: true
        preventStealing: true
        cursorShape: root.verticalBar ? Qt.SplitHCursor : Qt.SplitVCursor

        // Global coordinates avoid the resize-feedback loop caused by a handle
        // moving under a local mouseX while its parent width changes.
        property real pressGlobal: 0

        function globalCoord() {
            const pos = dragArea.mapToGlobal(Qt.point(mouseX, mouseY))
            return root.verticalBar ? pos.x : pos.y
        }

        onPositionChanged: {
            if (pressed)
                root.dragged(globalCoord() - pressGlobal)
        }

        onPressed: {
            pressGlobal = globalCoord()
            root.dragStarted()
        }
        onReleased: root.dragEnded()
        onCanceled: root.dragEnded()
    }
}
