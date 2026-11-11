import QtQuick
import QtQuick.Layouts
import SpanTui

// Standard pane header bar: paneHeaderH strip with a leading title (text via
// `title`, or custom content via the `leading` alias), trailing controls in
// the default `actions` slot, and an optional whole-header drag gesture for
// tile rearranging. Pair it with a bottom PixelLine in the host layout.
Item {
    id: root

    property string title: ""
    property string titleTone: "default"

    // Whole-header drag gesture; positions are delivered in global coords.
    property bool dragEnabled: false
    readonly property bool dragging: headerMouse.dragArmed
    readonly property bool hovered: headerMouse.containsMouse

    // Custom leading content replacing `title` (e.g. a kind-switched Loader).
    property alias leading: leadingHost.data
    // Trailing controls — declare ChromeButton children here.
    default property alias actions: actionHost.data

    signal pressed()
    signal dragBegan(point globalPos)
    signal dragMoved(point globalPos)
    signal dragEnded(point globalPos)
    signal dragCanceled()

    // Squared px distance before a press arms into a drag.
    readonly property int dragThresholdSq: 36   // style-ok: drag arm distance

    implicitHeight: Theme.paneHeaderH

    MouseArea {
        id: headerMouse
        anchors.fill: parent
        z: Theme.zBelow
        hoverEnabled: true
        property point pressPos
        property bool dragArmed: false
        onPressed: (mouse) => {
            root.pressed()
            pressPos = Qt.point(mouse.x, mouse.y)
            dragArmed = false
        }
        onPositionChanged: (mouse) => {
            if (!pressed || !root.dragEnabled)
                return
            const dx = mouse.x - pressPos.x
            const dy = mouse.y - pressPos.y
            if (!dragArmed && (dx * dx + dy * dy) >= root.dragThresholdSq) {
                dragArmed = true
                root.dragBegan(mapToGlobal(Qt.point(mouse.x, mouse.y)))
            } else if (dragArmed) {
                root.dragMoved(mapToGlobal(Qt.point(mouse.x, mouse.y)))
            }
        }
        onReleased: (mouse) => {
            if (dragArmed)
                root.dragEnded(mapToGlobal(Qt.point(mouse.x, mouse.y)))
            dragArmed = false
        }
        onCanceled: {
            if (dragArmed)
                root.dragCanceled()
            dragArmed = false
        }
    }

    RowLayout {
        anchors.fill: parent
        anchors.leftMargin: Theme.spaceMd
        anchors.rightMargin: Theme.spaceMd
        spacing: Theme.spaceSm

        Item {
            id: leadingHost
            Layout.fillWidth: true
            Layout.fillHeight: true

            UiText {
                anchors.fill: parent
                visible: root.title.length > 0
                text: root.title
                size: "md"
                tone: root.titleTone
                elide: Text.ElideRight
                verticalAlignment: Text.AlignVCenter
            }
        }

        Row {
            id: actionHost
            spacing: Theme.spaceSm
            Layout.alignment: Qt.AlignVCenter
        }
    }
}
