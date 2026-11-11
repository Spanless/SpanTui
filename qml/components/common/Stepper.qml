pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Layouts
import QtQuick.Window
import SpanTui

// Numeric stepper as one compound field on a sunken track — the same track
// recipe as SegmentedControl: minus cell | value well | plus cell. The cells
// are bare glyphs: hover only darkens the glyph (as on segments), a press
// shows a track tint as action feedback; nothing is raised because nothing
// is selected. The value well stretches when the
// control fills a slot; editing it puts the focus ring on the track.
Item {
    id: root

    property int value: 0
    property int minimumValue: 0
    property int maximumValue: 100
    // Value well floor width — wide enough for two digits at uiMd.
    readonly property int valueW: 52
    readonly property real renderDpr: PixelGrid.safeDpr(Screen.devicePixelRatio)

    // Requested value from a ± click or a typed commit in the value well —
    // already clamped to [min, max]; the caller writes it back to `value`.
    signal valueEdited(int value)

    implicitHeight: Theme.sizeMd
    implicitWidth: Theme.fieldMd

    onValueChanged: if (!valueEdit.activeFocus)
                        valueEdit.text = String(value)
    Component.onCompleted: valueEdit.text = String(value)

    function clamp(n) {
        return Math.max(minimumValue, Math.min(maximumValue, n))
    }

    function stepBy(delta) {
        const next = clamp(value + delta)
        if (next !== value)
            valueEdited(next)
    }

    CrispSurface {
        anchors.fill: parent
        devicePixelRatio: root.renderDpr
        radius: Theme.radiusMd
        elevation: "sunken"
        focusRing: valueEdit.activeFocus
    }

    RowLayout {
        id: row
        anchors.fill: parent
        spacing: 0

        StepperCell {
            Layout.fillHeight: true
            Layout.preferredWidth: root.implicitHeight
            iconName: "minus"
            tip: qsTr("Decrease")
            enabled: root.value > root.minimumValue
            onClicked: root.stepBy(-1)
        }

        Item {
            Layout.fillWidth: true
            Layout.fillHeight: true
            Layout.minimumWidth: root.valueW

            // The well is a real input: click → select-all → type → Enter or
            // blur commits (clamped); Esc reverts. No Binding on `text` — a
            // binding would swallow typed text before the commit reads it.
            TextInput {
                id: valueEdit
                anchors.fill: parent
                anchors.leftMargin: Theme.spaceSm
                anchors.rightMargin: Theme.spaceSm
                horizontalAlignment: TextInput.AlignHCenter
                verticalAlignment: TextInput.AlignVCenter
                color: Theme.text
                selectionColor: Theme.textSelection
                selectedTextColor: Theme.text
                selectByMouse: true
                clip: true
                font: Appearance.uiFont(Theme.uiMd, Appearance.fontRevision)

                function commitValue() {
                    const n = parseInt(text, 10)
                    if (isNaN(n)) {
                        text = String(root.value)  // invalid rolls back
                        return
                    }
                    const clamped = root.clamp(n)
                    text = String(clamped)
                    if (clamped !== root.value)
                        root.valueEdited(clamped)
                }

                onActiveFocusChanged: {
                    if (activeFocus) {
                        selectAll()
                    } else {
                        commitValue()
                    }
                }
                Keys.onReturnPressed: commitValue()
                Keys.onEnterPressed: commitValue()
                Keys.onEscapePressed: text = String(root.value)

                HoverHandler { cursorShape: Qt.IBeamCursor }
            }
        }

        StepperCell {
            Layout.fillHeight: true
            Layout.preferredWidth: root.implicitHeight
            iconName: "plus"
            tip: qsTr("Increase")
            enabled: root.value < root.maximumValue
            onClicked: root.stepBy(1)
        }
    }

    // One cell at the track's end: a bare glyph whose press tint is inset
    // by thumbInset, so its radiusSm stays concentric with the radiusMd track.
    component StepperCell: Item {
        id: cell
        property string iconName: ""
        property string tip: ""
        readonly property bool engaged: enabled && (cellMouse.containsMouse
                                                    || cellMouse.pressed)
        signal clicked()

        opacity: enabled ? 1 : Theme.opacityDisabled

        CrispSurface {
            anchors.fill: parent
            anchors.margins: Theme.thumbInset
            devicePixelRatio: root.renderDpr
            radius: Theme.radiusSm
            fillEnabled: cell.enabled && cellMouse.pressed
            fillTopColor: Theme.pressedOnInset
        }

        Icon {
            anchors.centerIn: parent
            name: cell.iconName
            size: Theme.iconSm
            color: cell.engaged ? Theme.text : Theme.muted
        }

        MouseArea {
            id: cellMouse
            anchors.fill: parent
            hoverEnabled: true
            cursorShape: Qt.PointingHandCursor
            onClicked: cell.clicked()
        }

        Tip {
            text: cell.tip
            visible: cell.tip.length > 0 && cellMouse.containsMouse
        }
    }
}
