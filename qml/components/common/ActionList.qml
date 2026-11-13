pragma ComponentBehavior: Bound

import QtQuick
import SpanTui

// Shared action rows for context menus.
// The owner decides when to close; this list only presents and fires.
//
// Hover is one shared GlideHighlight pill that glides between rows instead
// of a per-row fill (rows keep only their pressed fill). The pill is a solid
// accent fill (danger for danger rows) and the row it sits on switches its
// label to accentText/dangerText; a pressed row sinks through the shared
// solidFill recipe (accentPressed / dangerPressed). Separator and disabled rows are not glide
// targets — the pill stays on the last row, matching pointer-in-a-gap
// behavior.
//
// A row whose action carries a non-empty `submenu` array shows a trailing
// chevron; the host opens that array as a nested list (AppContextMenu).
//
// implicitHeight is arithmetic, not column.implicitHeight: positioner
// relayout runs on a deferred polish pass, so a popup auto-size snapshot
// taken in the same turn as a model swap (AppPopup._applyAutoSize) would see
// fresh delegates still stacked at y=0 and read one row's height.
Item {
    id: root

    property var actions: []
    property real rowWidth: 210
    // Host binds the popup's visibility here; each false→true flip plays the
    // staggered row entrance (row contents rise and fade in top-down).
    property bool revealed: true
    // Rows past this index enter together so long menus never feel slow.
    readonly property int staggerRows: 8
    readonly property real renderDpr: PixelGrid.safeDpr(Screen.devicePixelRatio)

    // `row` is the delegate, for hosts that place a submenu beside it.
    signal triggered(var action, Item row)
    // The pointer entered an enabled action row.
    signal rowHovered(var action, Item row)

    readonly property real separatorHeight: Theme.spaceMd
                                            + PixelGrid.hairline(renderDpr)
    readonly property real contentHeight: {
        let total = 0
        for (let i = 0; i < actions.length; ++i)
            total += (actions[i] && actions[i].separator === true)
                     ? separatorHeight : Theme.sizeMd
        return total + Math.max(0, actions.length - 1) * Theme.spaceSm
    }

    implicitWidth: rowWidth
    implicitHeight: contentHeight
    width: implicitWidth
    height: implicitHeight

    // Rows climb past the column to this root for the glide context —
    // declared, not injected: QML cannot add dynamic properties to an
    // object at runtime.
    property var glideContext: glide.glideEnabled ? glide : null

    // Rebuilt content invalidates the pill — re-snap on the next hover
    // instead of sliding over from a stale row rect.
    onActionsChanged: glide.reset()

    GlideHighlight {
        id: glide
        target: column
        fillColor: Theme.accent
        dangerFillColor: Theme.danger
    }

    Column {
        id: column
        width: parent.width
        spacing: Theme.spaceSm

        Repeater {
            model: root.actions

            delegate: Item {
                id: actionRow

                required property var modelData
                required property int index
                // Entrance progress (0 → 1). Only the row contents move — the
                // row rect itself stays put so the glide pill never maps to a
                // shifted frame mid-entrance.
                property real enter: 1
                readonly property bool separator: modelData.separator === true
                readonly property bool itemEnabled: modelData.enabled === undefined || modelData.enabled
                readonly property bool danger: modelData.danger === true
                // Not Array.isArray: a nested array read through a model
                // element arrives as a sequence wrapper, which is not an Array.
                readonly property bool hasSubmenu: !!modelData.submenu
                                                   && modelData.submenu.length > 0
                // GlideHighlight row contract: enabled action rows are
                // targets; separators and disabled rows are skipped.
                readonly property bool glideRow: !separator && itemEnabled
                readonly property bool glideDanger: danger
                readonly property bool onFill: glide.currentRow === actionRow
                                               || itemMouse.pressed
                readonly property color labelColor: onFill
                    ? (danger ? Theme.dangerText : Theme.accentText)
                    : (danger ? Theme.danger : Theme.text)

                width: root.rowWidth
                height: separator ? root.separatorHeight : Theme.sizeMd

                PixelLine {
                    visible: actionRow.separator
                    anchors.left: parent.left
                    anchors.right: parent.right
                    anchors.verticalCenter: parent.verticalCenter
                    anchors.leftMargin: Theme.spaceMd
                    anchors.rightMargin: Theme.spaceMd
                    height: implicitHeight
                    lineColor: Theme.separator
                    opacity: Theme.fadeMd * actionRow.enter
                }

                // Press feedback only — hover chrome rides the shared pill.
                CrispSurface {
                    visible: !actionRow.separator
                    anchors.fill: parent
                    devicePixelRatio: root.renderDpr
                    radius: Theme.radiusSm
                    fillTopColor: actionRow.danger ? Theme.dangerPressed
                                                   : Theme.accentPressed
                    fillMiddleColor: fillTopColor
                    fillBottomColor: fillTopColor
                    strokeEnabled: false
                    shadowEnabled: false
                    opacity: itemMouse.pressed ? 1 : 0
                }

                Row {
                    visible: !actionRow.separator
                    anchors.fill: parent
                    anchors.leftMargin: Theme.spaceMd
                    anchors.rightMargin: Theme.spaceMd
                                         + (actionRow.hasSubmenu ? Theme.iconSm + Theme.spaceSm : 0)
                    spacing: Theme.spaceSm
                    opacity: (actionRow.itemEnabled ? 1 : Theme.opacityDisabled)
                             * actionRow.enter
                    transform: Translate { y: (1 - actionRow.enter) * Theme.spaceSm }

                    // `programIcon`: a program's own full-color icon in
                    // place of a catalog glyph (e.g. one entry per shell).
                    ProgramIcon {
                        visible: path.length > 0
                        anchors.verticalCenter: parent.verticalCenter
                        path: actionRow.modelData.programIcon || ""
                        size: Theme.iconMd
                    }

                    Icon {
                        visible: !(actionRow.modelData.programIcon)
                        anchors.verticalCenter: parent.verticalCenter
                        name: actionRow.modelData.icon || ""
                        size: Theme.iconMd
                        color: actionRow.onFill || actionRow.danger
                               ? actionRow.labelColor : Theme.muted
                        hovered: actionRow.onFill
                        pressed: itemMouse.pressed
                        // The pill is a solid accent/danger fill — a tinted
                        // part would blend into it.
                        tinted: !actionRow.onFill
                    }

                    UiText {
                        anchors.verticalCenter: parent.verticalCenter
                        width: parent.width - x
                        text: actionRow.modelData.text || ""
                        color: actionRow.labelColor
                        size: "md"
                        elide: Text.ElideRight
                    }
                }

                Icon {
                    visible: actionRow.hasSubmenu
                    anchors.right: parent.right
                    anchors.rightMargin: Theme.spaceMd
                    anchors.verticalCenter: parent.verticalCenter
                    name: "chevron-right"
                    size: Theme.iconSm
                    color: actionRow.onFill ? actionRow.labelColor : Theme.muted
                    tinted: !actionRow.onFill
                    opacity: (actionRow.itemEnabled ? 1 : Theme.opacityDisabled)
                             * actionRow.enter
                }

                MouseArea {
                    id: itemMouse
                    visible: !actionRow.separator
                    anchors.fill: parent
                    enabled: actionRow.itemEnabled
                    hoverEnabled: true
                    cursorShape: enabled ? Qt.PointingHandCursor : Qt.ArrowCursor
                    onClicked: root.triggered(actionRow.modelData, actionRow)
                    onContainsMouseChanged: if (containsMouse)
                                                root.rowHovered(actionRow.modelData, actionRow)
                }

                Connections {
                    target: root
                    function onRevealedChanged() {
                        if (root.revealed && !Appearance.reduceMotion)
                            entrance.restart()
                        else if (!root.revealed) {
                            entrance.stop()
                            actionRow.enter = 1
                        }
                    }
                }

                SequentialAnimation {
                    id: entrance
                    PropertyAction { target: actionRow; property: "enter"; value: 0 }
                    PauseAnimation { duration: Math.min(actionRow.index, root.staggerRows) * Theme.durStagger }
                    NumberAnimation {
                        target: actionRow
                        property: "enter"
                        to: 1
                        duration: Theme.durMid
                        easing.type: Easing.BezierSpline
                        easing.bezierCurve: Theme.easeEmphasized
                    }
                }
            }
        }
    }
}
