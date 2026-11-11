pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import SpanTui

AnchorPopup {
    id: root

    // Ticks every 30s while the popup is open so "x min ago" stays current.
    property int ageRevision: 0

    function formatAge(timestampMs) {
        const elapsed = Math.max(0, (Date.now() - Number(timestampMs || 0)) / 1000)
        if (elapsed < 60)
            return qsTr("Just now")
        if (elapsed < 3600)
            return qsTr("%1 min ago").arg(Math.floor(elapsed / 60))
        if (elapsed < 86400)
            return qsTr("%1 h ago").arg(Math.floor(elapsed / 3600))
        return Qt.formatDateTime(new Date(timestampMs), "MM-dd hh:mm")
    }

    implicitHeight: Theme.dialogMd
    heading: qsTr("Messages")
    emptyText: qsTr("No messages")
    model: MessageCenter
    glideHover: true
    glideFillColor: Theme.hoverOnOverlay

    onOpening: {
        MessageCenter.inboxOpen = true
        ageTicker.restart()
        Qt.callLater(function() { listView.forceActiveFocus() })
    }
    onPopupClosed: {
        MessageCenter.inboxOpen = false
        ageTicker.stop()
    }

    Timer {
        id: ageTicker
        interval: 30000
        repeat: true
        // forceLayout() only repositions delegates; the age labels need an
        // observable dependency to re-evaluate.
        onTriggered: root.ageRevision += 1
    }

    headerActions: ChromeButton {
        size: "sm"
        kind: "ghost"
        text: qsTr("Clear all")
        enabled: MessageCenter.count > 0
        onClicked: MessageCenter.clear()
    }

    delegate: Item {
        id: row
        required property int index
        required property string kind
        required property string title
        required property string body
        required property var timestamp
        readonly property bool glideRow: true
        property bool expanded: false
        readonly property int collapsedBodyLines: 3
        readonly property int unboundedBodyLines: 2147483647
        readonly property string detailsText:
            body.length > 0 ? title + "\n" + body : title


        width: ListView.view ? ListView.view.width : 0
        implicitHeight: rowContent.implicitHeight + Theme.spaceMd * 2
        height: implicitHeight

        Behavior on height {
            enabled: !Appearance.reduceMotion
            NumberAnimation {
                duration: Theme.durMid
                easing.type: Easing.OutCubic
            }
        }

        RowLayout {
            id: rowContent
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.top: parent.top
            anchors.leftMargin: Theme.spaceLg
            anchors.rightMargin: Theme.spaceLg
            anchors.topMargin: Theme.spaceMd
            spacing: Theme.spaceMd

            StatusIcon {
                Layout.alignment: Qt.AlignTop
                Layout.topMargin: 1
                kind: row.kind
                size: Theme.iconSm
            }

            ColumnLayout {
                Layout.fillWidth: true
                spacing: Theme.spaceSm

                RowLayout {
                    Layout.fillWidth: true
                    spacing: Theme.spaceMd

                    UiText {
                        Layout.fillWidth: true
                        text: row.title
                        size: "md"
                        elide: Text.ElideRight
                    }
                    UiText {
                        text: {
                            const _age = root.ageRevision
                            return root.formatAge(row.timestamp)
                        }
                        tone: "muted"
                        size: "sm"
                    }
                }
                UiText {
                    id: bodyText
                    Layout.fillWidth: true
                    visible: row.body.length > 0
                    text: row.body
                    tone: "muted"
                    size: "md"
                    wrapMode: Text.WordWrap
                    maximumLineCount: row.expanded
                                      ? row.unboundedBodyLines
                                      : row.collapsedBodyLines
                    elide: row.expanded ? Text.ElideNone : Text.ElideRight
                }
                RowLayout {
                    Layout.fillWidth: true
                    spacing: Theme.spaceSm

                    ChromeButton {
                        Layout.alignment: Qt.AlignVCenter
                        Layout.leftMargin: -Theme.spaceMd
                        visible: bodyText.truncated || row.expanded
                        size: "sm"
                        kind: "ghost"
                        text: row.expanded ? qsTr("Collapse") : qsTr("Expand all")
                        onClicked: row.expanded = !row.expanded
                    }
                    Item {
                        Layout.fillWidth: true
                    }
                    ChromeButton {
                        Layout.alignment: Qt.AlignVCenter
                        size: "sm"
                        kind: "ghost"
                        iconName: "copy"
                        tip: qsTr("Copy message details")
                        onClicked: Clipboard.copyText(row.detailsText)
                    }
                }
            }
        }

        PixelLine {
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.bottom: parent.bottom
            lineColor: Theme.separator
            opacity: Theme.fadeSm
            visible: row.index < MessageCenter.count - 1
        }
    }
}
