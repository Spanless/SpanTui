pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Layouts
import SpanTui

// The confirmed update step by step (download or copy, verify, start Setup),
// each with its own progress. Hiding the card leaves the update running in
// the sidebar's task row; after a failure it shows why until it is closed.
// SpanTui quits as soon as Setup has started, so success needs no state.
AppDialog {
    id: root

    readonly property bool failed: !AppUpdate.applying && AppUpdate.applyError.length > 0

    heading: failed ? qsTr("Update failed") : qsTr("Updating SpanTui")
    message: qsTr("%1  →  %2").arg(AppUpdate.currentVersion).arg(AppUpdate.availableVersion)
    cancelText: failed ? qsTr("Close") : qsTr("Hide")
    onCancelled: root.close()

    ColumnLayout {
        Layout.fillWidth: true
        spacing: Theme.spaceMd

        Repeater {
            model: AppUpdate.applySteps

            RowLayout {
                id: stepRow
                required property int index
                required property string modelData
                readonly property bool done: index < AppUpdate.applyStep
                readonly property bool current: index === AppUpdate.applyStep

                Layout.fillWidth: true
                spacing: Theme.spaceMd

                // Finished and running steps carry a badge, the ones still
                // ahead their number.
                Item {
                    implicitWidth: Theme.iconSm
                    implicitHeight: Theme.iconSm

                    StatusIcon {
                        anchors.centerIn: parent
                        visible: stepRow.done || stepRow.current
                        size: Theme.iconSm
                        kind: stepRow.done ? "success" : root.failed ? "error" : "busy"
                    }
                    UiText {
                        anchors.centerIn: parent
                        visible: !stepRow.done && !stepRow.current
                        text: String(stepRow.index + 1)
                        tone: "muted"
                        size: "sm"
                    }
                }
                UiText {
                    Layout.fillWidth: true
                    text: stepRow.modelData
                    tone: stepRow.done || stepRow.current ? "default" : "muted"
                    elide: Text.ElideRight
                }
                UiText {
                    text: {
                        if (stepRow.done)
                            return qsTr("Done")
                        if (!stepRow.current)
                            return ""
                        if (root.failed)
                            return qsTr("Failed")
                        if (AppUpdate.applyStepProgress < 0)
                            return qsTr("In progress")
                        return qsTr("%1%").arg((AppUpdate.applyStepProgress * 100).toFixed(1))
                    }
                    tone: stepRow.current && root.failed ? "danger" : "muted"
                    size: "sm"
                }
            }
        }

        OverflowProgressBar {
            Layout.fillWidth: true
            Layout.topMargin: Theme.spaceSm
            visible: !root.failed
            barWidth: width
            barHeight: 6
            progress: AppUpdate.applyStepProgress
            motionActive: root.opened && AppUpdate.applying
        }

        UiText {
            Layout.fillWidth: true
            visible: text.length > 0
            text: root.failed ? AppUpdate.applyError : AppUpdate.applyDetail
            tone: root.failed ? "danger" : "muted"
            size: "sm"
            wrapMode: Text.WordWrap
        }

        UiText {
            Layout.fillWidth: true
            visible: !root.failed
            text: qsTr("SpanTui closes once the installer starts. The installer shows its own progress and opens the new version when it is done.")
            tone: "muted"
            size: "sm"
            wrapMode: Text.WordWrap
        }
    }
}
