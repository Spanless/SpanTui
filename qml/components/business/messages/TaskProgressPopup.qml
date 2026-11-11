import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import SpanTui

AnchorPopup {
    id: root

    // Per-row height estimate for the height clamp; the list itself stays
    // delegate-sized once opened.
    readonly property int rowEstimate: 58

    implicitHeight: Theme.paneHeaderH
                    + Math.max(1, TaskProgress.count) * rowEstimate
                    + Theme.spaceMd
    heading: qsTr("Tasks")
    emptyText: qsTr("No running tasks")
    model: TaskProgress

    onOpening: TaskProgress.popupOpen = true
    onPopupClosed: TaskProgress.popupOpen = false

    headerActions: UiText {
        visible: TaskProgress.activeCount > 0
        text: qsTr("%1 running").arg(TaskProgress.activeCount)
        tone: "muted"
        size: "sm"
    }

    delegate: Item {
        id: row
        required property int index
        required property string title
        required property string detail
        required property real progress
        required property bool determinate
        required property bool overflow
        required property bool running
        required property string source
        required property int outcome

        width: ListView.view ? ListView.view.width : 0
        implicitHeight: rowContent.implicitHeight + Theme.spaceMd * 2
        height: implicitHeight

        ColumnLayout {
            id: rowContent
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.top: parent.top
            anchors.leftMargin: Theme.spaceLg
            anchors.rightMargin: Theme.spaceLg
            anchors.topMargin: Theme.spaceMd
            spacing: Theme.spaceSm

            RowLayout {
                Layout.fillWidth: true
                spacing: Theme.spaceMd

                UiText {
                    Layout.fillWidth: true
                    text: row.title
                    size: "md"
                    wrapMode: Text.WordWrap
                    maximumLineCount: 2
                }
                UiText {
                    text: {
                        if (row.outcome === TaskProgress.Failed)
                            return qsTr("Failed")
                        if (row.outcome === TaskProgress.Cancelled)
                            return qsTr("Cancelled")
                        if (!row.determinate)
                            return qsTr("In progress")
                        return qsTr("%1%").arg(Math.round(row.progress * 100))
                    }
                    tone: row.outcome === TaskProgress.Failed
                          ? "danger"
                          : row.outcome === TaskProgress.Cancelled
                            ? "warning" : "muted"
                    size: "sm"
                }
                ChromeButton {
                    visible: !row.running
                    size: "sm"
                    kind: "ghost"
                    iconName: "x"
                    tip: qsTr("Dismiss")
                    onClicked: TaskProgress.dismiss(row.source)
                }
            }

            OverflowProgressBar {
                Layout.fillWidth: true
                barWidth: width
                barHeight: 6
                progress: row.progress
                indeterminate: row.running && !row.determinate
                motionActive: row.running
                fillColor: row.outcome === TaskProgress.Failed
                           ? Theme.danger
                           : row.outcome === TaskProgress.Cancelled
                             ? Theme.warning : Theme.accent
            }

            UiText {
                Layout.fillWidth: true
                visible: row.detail.length > 0
                text: row.detail
                tone: "muted"
                size: "sm"
                wrapMode: Text.WordWrap
                maximumLineCount: 2
            }
        }

        PixelLine {
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.bottom: parent.bottom
            lineColor: Theme.separator
            opacity: Theme.fadeSm
            visible: row.index < TaskProgress.count - 1
        }
    }
}
