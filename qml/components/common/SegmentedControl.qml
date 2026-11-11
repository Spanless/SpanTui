pragma ComponentBehavior: Bound

import QtQuick
import SpanTui

// `model` entries: {key, text?, iconName?, tip?}. multiSelect keeps its own
// checkedKeys, hides the thumb and reports toggled(key, on).
Item {
    id: root

    property var model: []
    property var current
    property bool multiSelect: false
    property var checkedKeys: []
    // "sm" | "md" — cell height tier; sm serves dense tool strips.
    property string size: "md"
    signal picked(var key)
    signal toggled(var key, bool checked)

    // Gap between the track edge and the thumbs; keeps them concentric.
    readonly property real trackPad: Theme.thumbInset
    readonly property int count: model.length
    readonly property real segmentW: count > 0 ? (width - trackPad * 2) / count : 0
    readonly property real renderDpr: PixelGrid.safeDpr(Screen.devicePixelRatio)
    readonly property bool reduceMotion: Appearance.reduceMotion

    implicitHeight: size === "sm" ? Theme.sizeSm : Theme.sizeMd
    // Natural width: square icon cells + text cells at content width, so
    // tool-strip placements without a fill slot don't collapse to zero.
    implicitWidth: segmentRow.implicitWidth + trackPad * 2

    function entryOf(d) {
        return (d !== null && typeof d === "object" && d.key !== undefined)
               ? d : { "key": d }
    }
    function indexOf(key) {
        for (let i = 0; i < model.length; ++i) {
            if (entryOf(model[i]).key === key)
                return i
        }
        return -1
    }
    function isChecked(key) {
        return checkedKeys.indexOf(key) >= 0
    }
    readonly property int currentIndex: indexOf(current)

    CrispSurface {
        anchors.fill: parent
        devicePixelRatio: root.renderDpr
        radius: Theme.radiusMd
        elevation: "sunken"
    }

    // Tracks the real cell (icon segments are square). Depends on
    // segments.count: itemAt() is null until the Repeater is done, and the
    // binding would never re-run.
    readonly property var currentCell: (root.currentIndex >= 0
                                        && segments.count > root.currentIndex)
                                       ? segments.itemAt(root.currentIndex)
                                       : null
    // Slide animations stay off until the indicator has landed once — the
    // first placement snaps instead of flying in from the track edge.
    property bool indicatorArmed: false
    Component.onCompleted: Qt.callLater(function() { root.indicatorArmed = true })

    CrispSurface {
        id: indicator
        // Hover paints no surface (the platform convention): only the
        // hovered label darkens, so the thumb is the one shape on the track.
        visible: !root.multiSelect && root.currentCell !== null
        x: root.currentCell ? root.trackPad + root.currentCell.x : root.trackPad
        y: root.trackPad
        width: root.currentCell ? root.currentCell.width : root.segmentW
        height: Math.max(0, parent.height - root.trackPad * 2)
        devicePixelRatio: root.renderDpr
        radius: Theme.radiusSm
        elevation: "raised"

        Behavior on x {
            enabled: !root.reduceMotion && root.indicatorArmed
            NumberAnimation { duration: Theme.durFast; easing.type: Easing.OutCubic }
        }
        Behavior on width {
            enabled: !root.reduceMotion && root.indicatorArmed
            NumberAnimation { duration: Theme.durFast; easing.type: Easing.OutCubic }
        }
    }

    Row {
        id: segmentRow
        anchors.fill: parent
        anchors.leftMargin: root.trackPad
        anchors.rightMargin: root.trackPad

        Repeater {
            id: segments
            model: root.model
            delegate: Item {
                id: segment
                clip: true
                required property var modelData
                required property int index
                readonly property var entry: root.entryOf(modelData)
                readonly property string tip: entry.tip !== undefined ? entry.tip : ""
                readonly property bool iconOnly: entry.iconName !== undefined
                                                 && entry.iconName.length > 0
                                                 && entry.text === undefined
                readonly property bool on: root.multiSelect
                                           ? root.isChecked(entry.key)
                                           : root.current === entry.key
                readonly property color labelColor:
                    on || segmentMouse.containsMouse ? Theme.text : Theme.muted

                // Icon-only segments stay square; text segments share the
                // width evenly. implicitWidth feeds the control's natural
                // width for unfilled placements.
                width: iconOnly ? height : root.segmentW
                height: parent.height
                implicitWidth: iconOnly
                               ? root.implicitHeight
                               : segmentContent.implicitWidth + Theme.spaceMd * 2

                Accessible.role: Accessible.Button
                Accessible.name: segment.entry.text !== undefined
                                 ? segment.entry.text
                                 : (segment.tip.length > 0 ? segment.tip
                                                           : String(segment.entry.key))
                Accessible.onPressAction: segment.activate()

                function activate() {
                    if (root.multiSelect) {
                        var keys = root.checkedKeys.slice()
                        var at = keys.indexOf(entry.key)
                        if (at >= 0) keys.splice(at, 1); else keys.push(entry.key)
                        root.checkedKeys = keys
                        root.toggled(entry.key, at < 0)
                    } else {
                        root.picked(entry.key)
                    }
                }

                // Press on an off segment: the sunken pressed tone, inset like
                // the thumb so it stays concentric with the track. Hover only
                // darkens the label (the in-track convention).
                CrispSurface {
                    anchors.fill: parent
                    anchors.topMargin: root.trackPad
                    anchors.bottomMargin: root.trackPad
                    anchors.leftMargin: root.trackPad / 2
                    anchors.rightMargin: root.trackPad / 2
                    visible: !segment.on && segmentMouse.pressed
                    devicePixelRatio: root.renderDpr
                    radius: Theme.radiusSm
                    fillTopColor: Theme.pressedOnInset
                    fillMiddleColor: fillTopColor
                    fillBottomColor: fillTopColor
                    strokeEnabled: false
                    shadowEnabled: false
                }

                // Multi-select cells carry their own raised thumb while on;
                // exclusive mode's selection rides the shared sliding
                // indicator.
                CrispSurface {
                    anchors.fill: parent
                    anchors.topMargin: root.trackPad
                    anchors.bottomMargin: root.trackPad
                    anchors.leftMargin: root.trackPad / 2
                    anchors.rightMargin: root.trackPad / 2
                    visible: root.multiSelect && segment.on
                    devicePixelRatio: root.renderDpr
                    radius: Theme.radiusSm
                    elevation: "raised"
                    fillTopColor: segmentMouse.pressed ? Theme.raisedFillPressed
                                                       : Theme.raisedFillTop
                    fillMiddleColor: segmentMouse.pressed ? Theme.raisedFillPressed
                                                          : Theme.raisedFill
                }

                Row {
                    id: segmentContent
                    anchors.centerIn: parent
                    spacing: Theme.spaceSm

                    Icon {
                        id: leadIcon
                        visible: segment.entry.iconName !== undefined
                                 && segment.entry.iconName.length > 0
                        anchors.verticalCenter: parent.verticalCenter
                        name: visible ? segment.entry.iconName : ""
                        size: Theme.iconSm
                        color: segment.labelColor
                        hovered: segmentMouse.containsMouse
                        pressed: segmentMouse.pressed
                        active: segment.on
                    }
                    UiText {
                        anchors.verticalCenter: parent.verticalCenter
                        // Icon-only entries carry no label — falling back to
                        // the key would render the raw key as visible text.
                        visible: !segment.iconOnly
                        text: visible
                              ? (segment.entry.text !== undefined
                                 ? segment.entry.text
                                 : String(segment.entry.key))
                              : ""
                        size: "md"
                        emphasis: segment.on
                        color: segment.labelColor
                        elide: Text.ElideRight
                        // Elide needs a bounded width.
                        width: Math.min(implicitWidth, Math.max(0,
                                   segment.width - Theme.spaceMd * 2
                                   - (leadIcon.visible
                                      ? leadIcon.width + segmentContent.spacing
                                      : 0)))
                    }
                }

                MouseArea {
                    id: segmentMouse
                    anchors.fill: parent
                    hoverEnabled: true
                    cursorShape: Qt.PointingHandCursor
                    onClicked: segment.activate()
                }

                Tip {
                    text: segment.tip
                    visible: segment.tip.length > 0 && segmentMouse.containsMouse
                }
            }
        }
    }
}
