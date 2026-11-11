import QtQuick
import SpanTui

// Base row for every list. Hover/press colours snap: a ColorAnimation on
// dense lists flashes.
Item {
    id: root

    property bool selected: false
    property bool expandable: false
    // Set by AppContextMenu while open; an accent rim independent of selection.
    property bool menuTarget: false
    property bool glideRow: true
    // For rows outside any glideContext ancestor (see GlideHighlight).
    property var glideHost: null
    function _glideCtx() {
        if (glideHost)
            return glideHost
        let node = parent
        while (node) {
            // Bracket access: intermediate items lack the property.
            if (node["glideContext"] !== undefined)
                return node["glideContext"]
            node = node.parent
        }
        return null
    }
    // Called from the fill bindings, not cached: glideContext has no notify
    // signal, so it is re-walked on every state change.
    function _glideState() {
        const ctx = _glideCtx()
        return {
            managed: ctx !== null && ctx.enabled === true,
            active: ctx !== null && ctx.active === true
        }
    }
    // Toggles on press: MouseArea drops the second click of a double-click.
    // Rows with an interactive chevron turn it off and toggle there.
    property bool toggleOnPress: true
    property color categoryColor: "transparent"
    property int acceptedButtons: Qt.LeftButton | Qt.RightButton
    property bool hoverEnabled: true
    property int cursorShape: Qt.ArrowCursor
    readonly property bool hovered: mouse.containsMouse
    readonly property real renderDpr: PixelGrid.safeDpr(Screen.devicePixelRatio)
    readonly property real hairline: PixelGrid.hairline(renderDpr)
    property alias mouseArea: mouse

    // Inside a ListView delegate the view's itemHeight/itemPadX win over Theme.
    readonly property var hostView: ListView.view
    property int rowHeight: (hostView && hostView.itemHeight) || Theme.sizeMd
    readonly property int contentPadX: (hostView && hostView.itemPadX) || Theme.spaceMd
    readonly property int contentGap: Theme.spaceSm
    readonly property int contentIcon: Theme.iconMd

    signal clicked(var mouse)
    signal doubleClicked(var mouse)
    signal pressed(var mouse)
    signal toggled()

    height: rowHeight
    implicitHeight: rowHeight

    // A selected row keeps its fill under the glide pill and the pointer.
    CrispSurface {
        id: rowSurface
        anchors.fill: parent
        anchors.leftMargin: Theme.spaceSm
        anchors.rightMargin: Theme.spaceSm
        anchors.topMargin: root.hairline
        anchors.bottomMargin: root.hairline
        devicePixelRatio: root.renderDpr
        radius: Theme.radiusSm
        fillEnabled: {
            const glide = root._glideState()
            return mouse.pressed
                   || root.selected
                   || (root.hovered && !glide.managed)
                   || (root.categoryColor.a > 0
                       && !(root.hovered && glide.active))
        }
        fillTopColor: {
            const glide = root._glideState()
            if (mouse.pressed)
                return root.selected ? Theme.selectionPressed : Theme.pressed
            if (root.selected)
                return root.hovered ? Theme.selectionHover : Theme.selection
            if (root.hovered && !glide.managed)
                return Theme.hover
            return Theme.mix(Theme.surfaceRaised, root.categoryColor, 0.4)
        }
        fillMiddleColor: fillTopColor
        fillBottomColor: fillTopColor
        strokeEnabled: root.menuTarget
        strokeColor: Theme.accent
        strokeWidthPx: Math.max(1, Math.round(root.renderDpr))
        shadowEnabled: false
        opacity: rowSurface.fillEnabled || root.menuTarget ? 1 : 0
    }

    MouseArea {
        id: mouse
        anchors.fill: parent
        acceptedButtons: root.acceptedButtons
        hoverEnabled: root.hoverEnabled
        cursorShape: root.cursorShape
        onPressed: mouse => {
            root.pressed(mouse)
            if (root.expandable && root.toggleOnPress
                && mouse.button === Qt.LeftButton) {
                root.toggled()
            }
        }
        onClicked: mouse => root.clicked(mouse)
        onDoubleClicked: mouse => root.doubleClicked(mouse)
    }

    // After the MouseArea, so content handlers (TreeChevron) stay above it.
    default property alias content: body.data
    Item {
        id: body
        anchors.fill: parent
        anchors.leftMargin: root.contentPadX
        anchors.rightMargin: root.contentPadX
    }

}
