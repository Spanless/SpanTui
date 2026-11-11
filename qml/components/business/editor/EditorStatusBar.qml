import QtQuick
import QtQuick.Layouts
import SpanTui

Item {
    id: root

    property string fileName: ""
    property int cursorLine: 1
    property int cursorColumn: 1
    property int selectionLength: 0

    readonly property real renderDpr: PixelGrid.safeDpr(Screen.devicePixelRatio)

    height: PixelGrid.snapSize(Math.max(22, Theme.uiFontSize + 10), renderDpr)

    PixelLine {
        anchors.top: parent.top
        anchors.left: parent.left
        anchors.right: parent.right
        devicePixelRatio: root.renderDpr
        lineColor: Theme.separator
    }

    RowLayout {
        anchors.fill: parent
        anchors.leftMargin: Theme.spaceLg
        anchors.rightMargin: Theme.spaceMd
        spacing: Theme.spaceMd

        UiText {
            Layout.fillWidth: true
            text: root.fileName
            size: "sm"
            tone: "muted"
            elide: Text.ElideMiddle
        }
        UiText {
            visible: root.selectionLength > 0
            text: qsTr("%1 selected").arg(root.selectionLength)
            size: "sm"
            tone: "muted"
        }
        UiText {
            text: qsTr("Ln %1, Col %2").arg(root.cursorLine).arg(root.cursorColumn)
            size: "sm"
            tone: "muted"
        }
    }
}
