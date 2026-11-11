import QtQuick
import QtQuick.Layouts
import SpanTui

// Settings form item inside a SettingsSection. The fixed-width label cell
// lines every row's control up on one edge. The row owns only a bottom pad;
// `hint` renders inside it, so a note never moves the control.
Item {
    id: root

    property string title: ""
    property string description: ""
    // Under-control note line: "" hides it.
    property string hint: ""
    // "muted" | "warning" | "danger" | "success" — UiText tone names.
    property string hintTone: "muted"
    default property alias content: slot.data

    Layout.fillWidth: true

    // A taller hint font grows the pad, only while a hint shows: an empty
    // Text still reports one line of height.
    readonly property int hintBand:
        Math.max(Theme.spaceLg,
                 hint.length > 0 ? hintText.implicitHeight + 2 : 0) // style-ok: 1px air above/below the hint text

    implicitWidth: row.implicitWidth
    implicitHeight: row.implicitHeight + hintBand

    GridRow {
        id: row
        width: parent.width
        height: implicitHeight
        gutter: Theme.spaceLg

        GridCol {
            hug: true

            Item {
                implicitWidth: Theme.formLabelW
                implicitHeight: labelRow.implicitHeight
                width: parent.width

                RowLayout {
                    id: labelRow
                    width: parent.width
                    anchors.verticalCenter: parent.verticalCenter
                    spacing: Theme.spaceSm

                    Item {
                        Layout.preferredWidth: help.implicitWidth
                        Layout.preferredHeight: help.implicitHeight
                        Layout.alignment: Qt.AlignVCenter

                        HelpTooltip {
                            id: help
                            anchors.centerIn: parent
                            text: root.description
                        }
                    }

                    UiText {
                        Layout.fillWidth: true
                        Layout.preferredHeight: Theme.sizeMd
                        text: root.title
                        size: "md"
                        elide: Text.ElideRight
                        verticalAlignment: Text.AlignVCenter
                    }
                }
            }
        }

        GridCol {
            id: controlCol
            hug: true

            // Not a Column: a positioner measures only children it places.
            Item {
                width: parent.width
                implicitWidth: slot.implicitWidth
                implicitHeight: slot.implicitHeight

                RowLayout {
                    id: slot
                    width: parent.width
                    spacing: Theme.spaceMd
                }
            }
        }
    }

    // Fades instead of eliding: an elided Text shrinks contentWidth, so
    // min(contentWidth, cap) would loop.
    FadeText {
        id: hintText
        visible: root.hint.length > 0
        x: controlCol.x
        anchors.top: row.bottom
        width: controlCol.width
        height: root.hintBand
        text: root.hint
        tone: root.hintTone
        size: "hint"
        fadeColor: Theme.overlaySurface
    }
}
