pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Layouts
import SpanTui

// Remote tiles show the server, local tiles the terminal's process tree.
// Load level: worst metric, elevated from 60 %, high from 85 %.
RowLayout {
    id: root

    // The host's monitor for a remote tile, null for local.
    property RemoteSystemMonitor server: null
    property string sessionId: ""
    property bool running: false
    property bool focused: false
    property bool pinned: false

    readonly property bool remote: server !== null
    // Focus shows the summary; a pinned panel keeps it (and sampling) on.
    readonly property bool wanted: running && (focused || pinned)
    // Remote figures appear once the first reading arrived.
    readonly property bool shown: wanted && (!server || server.available)

    function formatMemory(bytes) {
        const value = Number(bytes || 0)
        if (value < 1024)
            return qsTr("%1 B").arg(Math.round(value))
        if (value < 1048576)
            return qsTr("%1 KB").arg((value / 1024).toFixed(value < 10240 ? 1 : 0))
        if (value < 1073741824)
            return qsTr("%1 MB").arg((value / 1048576).toFixed(value < 10485760 ? 1 : 0))
        return qsTr("%1 GB").arg((value / 1073741824).toFixed(2))
    }
    function formatRate(bytesPerSecond) {
        return qsTr("%1/s").arg(formatMemory(bytesPerSecond))
    }
    function formatPercent(value) {
        return qsTr("%1%").arg(Math.round(Math.max(0, value)))
    }
    function levelOf(percent) {
        return percent >= 85 ? 2 : percent >= 60 ? 1 : 0
    }
    function levelColor(level) {
        return level >= 2 ? Theme.danger : level === 1 ? Theme.warning : Theme.success
    }
    function toneOf(level) {
        return level >= 2 ? "danger" : level === 1 ? "warning" : "default"
    }
    // The detail panel colours a healthy figure too.
    function figureTone(level) {
        return level >= 2 ? "danger" : level === 1 ? "warning" : "success"
    }

    // Short load bar of the detail table: level-coloured fill on a tinted
    // track of the same hue.
    component LoadBar: Rectangle {
        property real percent: 0
        readonly property color fillColor: root.levelColor(root.levelOf(percent))
        Layout.alignment: Qt.AlignVCenter
        Layout.preferredWidth: Theme.sizeMd * 2
        Layout.preferredHeight: Theme.spaceSm + 2
        radius: height / 2
        color: Theme.withAlpha(fillColor, Theme.fadeSm)
        Rectangle {
            readonly property real share: Math.max(0, Math.min(1, parent.percent / 100))
            // A small share still shows as a dot rather than an empty track.
            width: share > 0 ? Math.max(parent.height, parent.width * share) : 0
            height: parent.height
            radius: parent.radius
            color: parent.fillColor
        }
    }

    property RemoteSystemMonitor consumedServer: null
    function syncConsumers() {
        ResourceMonitor.setConsumerVisible(root, wanted && !server)
        if (consumedServer && consumedServer !== server)
            consumedServer.setConsumerVisible(root, false)
        consumedServer = server
        if (server)
            server.setConsumerVisible(root, wanted)
    }
    onWantedChanged: syncConsumers()
    onServerChanged: syncConsumers()
    onRunningChanged: if (!running) {
        pinned = false
        detail.close()
    }
    Component.onCompleted: syncConsumers()
    Component.onDestruction: {
        ResourceMonitor.setConsumerVisible(root, false)
        if (consumedServer)
            consumedServer.setConsumerVisible(root, false)
    }

    readonly property var perf: {
        const _tick = ResourceMonitor.cpuPercent + ResourceMonitor.gpuPercent
        if (!wanted || server || sessionId.length === 0)
            return ({ cpuPercent: 0, memoryBytes: 0 })
        return ResourceMonitor.sessionMetrics(sessionId)
    }
    readonly property real cpuPercent: remote ? server.cpuPercent : Number(perf.cpuPercent || 0)
    readonly property real memoryBytes: remote ? Math.max(0, server.memoryUsedBytes)
                                               : Number(perf.memoryBytes || 0)
    readonly property real memoryTotal: remote ? server.memoryTotalBytes : ResourceMonitor.physicalMemoryBytes
    readonly property real memoryPercent: memoryTotal > 0 ? 100 * memoryBytes / memoryTotal : 0
    readonly property real diskPercent: remote && server.diskTotalBytes > 0
                                        ? 100 * Math.max(0, server.diskUsedBytes) / server.diskTotalBytes : 0
    readonly property int cpuLevel: levelOf(cpuPercent)
    readonly property int memoryLevel: levelOf(memoryPercent)
    readonly property int level: Math.max(cpuLevel, memoryLevel)
    // The server's memory reads as a share; a terminal's as its size.
    readonly property string memoryText: remote ? formatPercent(memoryPercent) : formatMemory(memoryBytes)

    spacing: Theme.spaceSm

    Item {
        id: summary
        Layout.alignment: Qt.AlignVCenter
        implicitWidth: summaryRow.implicitWidth + 2 * Theme.spaceMd
        implicitHeight: Theme.sizeSm

        Rectangle {
            anchors.fill: parent
            radius: Theme.radiusSm
            color: root.level >= 2 ? Theme.dangerSoft
                 : root.level === 1 ? Theme.warningSoft : "transparent"
        }
        Row {
            id: summaryRow
            anchors.centerIn: parent
            spacing: Theme.spaceSm
            Rectangle {
                readonly property real dotSize: Theme.spaceMd
                anchors.verticalCenter: parent.verticalCenter
                width: dotSize
                height: dotSize
                radius: dotSize / 2
                color: root.levelColor(root.level)
            }
            // Figures change width with every sample; each label holds the
            // width of its widest reading so the header never jitters.
            UiText {
                anchors.verticalCenter: parent.verticalCenter
                width: Math.max(implicitWidth, cpuProbe.implicitWidth)
                size: "sm"
                tone: root.toneOf(root.cpuLevel)
                text: qsTr("CPU %1").arg(root.formatPercent(root.cpuPercent))
            }
            UiText {
                anchors.verticalCenter: parent.verticalCenter
                size: "sm"
                tone: "muted"
                text: "·"
            }
            UiText {
                anchors.verticalCenter: parent.verticalCenter
                width: Math.max(implicitWidth, memoryProbe.implicitWidth)
                size: "sm"
                tone: root.toneOf(root.memoryLevel)
                text: qsTr("Mem %1").arg(root.memoryText)
            }
        }
    }

    // Width probes: never shown (an invisible item takes no layout slot),
    // they measure the widest reading of each figure in its own font.
    readonly property string widestPercent: formatPercent(100)
    readonly property string widestMemory: qsTr("%1 GB").arg("88.88")
    readonly property string widestRate: formatRate(888.8 * 1024)
    UiText { id: cpuProbe; visible: false; size: "sm"; text: qsTr("CPU %1").arg(root.widestPercent) }
    UiText {
        id: memoryProbe
        visible: false
        size: "sm"
        text: qsTr("Mem %1").arg(root.remote ? root.widestPercent : root.widestMemory)
    }
    UiText {
        id: valueProbe
        visible: false
        emphasis: true
        text: root.remote ? root.widestPercent : root.widestMemory
    }
    UiText { id: rateProbe; visible: false; text: "↓ " + root.widestRate }

    ChromeButton {
        id: dropButton
        size: "sm"
        kind: detail.opened ? "selected" : "ghost"
        iconName: "chevron-down"
        tip: root.remote ? qsTr("Server resources") : qsTr("Terminal resources")
        onClicked: detail.openBelow(this)
    }
    ChromeButton {
        size: "sm"
        kind: root.pinned ? "selected" : "ghost"
        iconName: "pin"
        tip: root.pinned ? qsTr("Unpin resource panel") : qsTr("Pin resource panel")
        onClicked: {
            root.pinned = !root.pinned
            if (root.pinned && !detail.opened)
                detail.openBelow(dropButton)
        }
    }

    AnchorPopup {
        id: detail
        // As wide as its figures, and only ever wider while open: detail
        // text that shrinks between samples must not pull the edge back.
        property real widest: 0
        popupWidth: Math.max(widest, Math.ceil(table.implicitWidth))
        heading: root.remote ? qsTr("Server resources") : qsTr("Terminal resources")
        alignRight: true
        // Sized by its content binding rather than a snapshot at open: rows
        // appear (network, disk) as the first readings land.
        autoSizeToContent: false
        width: implicitWidth
        height: implicitHeight
        // No focus: closing a focused popup returns focus to its opener,
        // yanking it from the pane whose click closed the panel.
        focus: false
        closeOnEscape: false
        closeOnOutside: !root.pinned
        // Per-frame anchor tracking would keep the window rendering for as
        // long as a pinned panel stays open; it follows samples and window
        // resizes instead.
        followAnchor: !root.pinned
        onOpening: widest = Math.ceil(table.implicitWidth)
        onPopupClosed: root.pinned = false
        readonly property bool showNetwork: root.remote && root.server.networkAvailable
        readonly property bool showDisk: root.remote && root.server.diskTotalBytes > 0

        // Rows: label, figure and a load bar in the metric's level colour,
        // then the detail.
        contentItem: ColumnLayout {
            id: table
            spacing: Theme.spaceSm
            onImplicitWidthChanged: if (detail.visible)
                                        detail.widest = Math.max(detail.widest, Math.ceil(implicitWidth))

            RowLayout {
                Layout.fillWidth: true
                Layout.leftMargin: Theme.spaceLg
                Layout.rightMargin: Theme.spaceLg
                Layout.topMargin: Theme.spaceMd
                spacing: Theme.spaceSm
                Icon {
                    name: root.remote ? "server" : "terminal"
                    size: Theme.iconSm
                    color: Theme.accent
                }
                UiText {
                    Layout.fillWidth: true
                    text: detail.heading
                    emphasis: true
                }
            }

            GridLayout {
                Layout.fillWidth: true
                Layout.leftMargin: Theme.spaceLg
                Layout.rightMargin: Theme.spaceLg
                Layout.bottomMargin: Theme.spaceMd
                columns: 4
                columnSpacing: Theme.spaceMd
                rowSpacing: Theme.spaceSm

                // CPU
                UiText { tone: "muted"; text: qsTr("CPU") }
                UiText {
                    Layout.minimumWidth: valueProbe.implicitWidth
                    horizontalAlignment: Text.AlignRight
                    emphasis: true
                    tone: root.figureTone(root.cpuLevel)
                    text: root.formatPercent(root.cpuPercent)
                }
                LoadBar { percent: root.cpuPercent }
                UiText {
                    tone: "muted"
                    text: root.remote
                          ? (root.server.loadAverage.length > 0 ? qsTr("Load %1").arg(root.server.loadAverage) : "")
                          : qsTr("Shell and its child processes")
                }
                // Memory
                UiText { tone: "muted"; text: qsTr("Memory") }
                UiText {
                    Layout.minimumWidth: valueProbe.implicitWidth
                    horizontalAlignment: Text.AlignRight
                    emphasis: true
                    tone: root.figureTone(root.memoryLevel)
                    text: root.remote ? root.formatPercent(root.memoryPercent) : root.formatMemory(root.memoryBytes)
                }
                LoadBar { percent: root.memoryPercent }
                UiText {
                    tone: "muted"
                    text: root.memoryTotal > 0
                          ? qsTr("%1 / %2").arg(root.formatMemory(root.memoryBytes)).arg(root.formatMemory(root.memoryTotal))
                          : ""
                }
                // Network (remote): rates only, no share to show as load.
                UiText { visible: detail.showNetwork; tone: "muted"; text: qsTr("Network") }
                Item { visible: detail.showNetwork }
                Item { visible: detail.showNetwork }
                Row {
                    visible: detail.showNetwork
                    spacing: Theme.spaceMd
                    UiText {
                        width: Math.max(implicitWidth, rateProbe.implicitWidth)
                        tone: "success"
                        text: root.remote ? "↓ " + root.formatRate(root.server.rxBytesPerSecond) : ""
                    }
                    UiText {
                        width: Math.max(implicitWidth, rateProbe.implicitWidth)
                        tone: "accent"
                        text: root.remote ? "↑ " + root.formatRate(root.server.txBytesPerSecond) : ""
                    }
                }
                // Root filesystem (remote)
                UiText { visible: detail.showDisk; tone: "muted"; text: qsTr("Disk /") }
                UiText {
                    visible: detail.showDisk
                    Layout.minimumWidth: valueProbe.implicitWidth
                    horizontalAlignment: Text.AlignRight
                    emphasis: true
                    tone: root.figureTone(root.levelOf(root.diskPercent))
                    text: root.formatPercent(root.diskPercent)
                }
                LoadBar {
                    visible: detail.showDisk
                    percent: root.diskPercent
                }
                UiText {
                    visible: detail.showDisk
                    tone: "muted"
                    text: root.remote
                          ? qsTr("%1 / %2").arg(root.formatMemory(Math.max(0, root.server.diskUsedBytes)))
                                           .arg(root.formatMemory(root.server.diskTotalBytes))
                          : ""
                }
            }
        }
    }

    // A pinned panel does not track its anchor per frame; it re-places
    // itself when a sample lands or the window resizes.
    function placePinned() {
        if (pinned && detail.opened)
            detail.reposition()
    }
    Connections {
        target: root.server
        enabled: root.remote
        function onSampleChanged() { root.placePinned() }
    }
    Connections {
        target: ResourceMonitor
        enabled: !root.remote
        function onMetricsChanged() { root.placePinned() }
    }
    // Window.window is an Item attached property: read it on root, not
    // inside the Connections object.
    readonly property Window hostWindow: Window.window
    Connections {
        target: root.hostWindow
        function onWidthChanged() { root.placePinned() }
        function onHeightChanged() { root.placePinned() }
    }
}
