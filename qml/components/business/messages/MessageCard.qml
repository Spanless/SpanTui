import QtQuick
import QtQuick.Layouts
import SpanTui

Item {
    id: root

    property string kind: "info"
    property string title: ""
    property string body: ""
    property bool dismissible: true
    property bool shown: true
    property string slideFrom: "top"

    signal dismissed()
    signal exitFinished()

    readonly property real renderDpr: PixelGrid.safeDpr(Screen.devicePixelRatio)
    readonly property bool reduceMotion: Appearance.reduceMotion
    onShownChanged: {
        if (shown) {
            exitTimer.stop()
        } else if (reduceMotion) {
            Qt.callLater(function() {
                if (!root.shown)
                    root.exitFinished()
            })
        } else {
            exitTimer.restart()
        }
    }

    implicitWidth: PixelGrid.snapSize(320, renderDpr)
    implicitHeight: content.implicitHeight + Theme.spaceLg * 2
    width: implicitWidth
    height: implicitHeight
    // Cards are created already `shown` — hold the presented pose for one
    // tick so the entrance plays instead of popping in at full opacity.
    property bool _entered: reduceMotion
    readonly property bool presentedPose: shown && _entered
    Component.onCompleted: {
        if (!_entered)
            Qt.callLater(function() { root._entered = true })
    }

    opacity: presentedPose ? 1 : 0
    visible: opacity > 0.01
    scale: presentedPose || reduceMotion ? 1 : 0.96

    transform: Translate {
        y: root.presentedPose || root.reduceMotion ? 0
           : (root.slideFrom === "bottom" ? Theme.spaceMd : -Theme.spaceMd)
        Behavior on y {
            enabled: !root.reduceMotion
            NumberAnimation {
                duration: Theme.durMid
                easing.type: root.presentedPose ? Easing.BezierSpline : Easing.InCubic
                easing.bezierCurve: Theme.easeEmphasized
            }
        }
    }

    Behavior on opacity {
        enabled: !root.reduceMotion
        NumberAnimation {
            duration: root.presentedPose ? Theme.durFast : Theme.durMid
            easing.type: root.presentedPose ? Easing.OutCubic : Easing.InCubic
        }
    }
    Behavior on scale {
        enabled: !root.reduceMotion
        NumberAnimation {
            duration: Theme.durMid
            easing.type: root.presentedPose ? Easing.BezierSpline : Easing.InCubic
            easing.bezierCurve: Theme.easeEmphasized
        }
    }
    Behavior on y {
        enabled: !root.reduceMotion
        NumberAnimation {
            duration: Theme.durMid
            easing.type: Easing.OutCubic
        }
    }
    Timer {
        id: exitTimer
        interval: Theme.durMid
        onTriggered: {
            if (!root.shown)
                root.exitFinished()
        }
    }

    PopupSurface {
        anchors.fill: parent
        devicePixelRatio: root.renderDpr
    }

    RowLayout {
        id: content
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: parent.top
        anchors.margins: Theme.spaceLg
        spacing: Theme.spaceMd

        StatusIcon {
            Layout.alignment: Qt.AlignTop
            Layout.topMargin: 1
            kind: root.kind
            size: Theme.iconMd
        }

        ColumnLayout {
            Layout.fillWidth: true
            spacing: Theme.spaceSm

            UiText {
                Layout.fillWidth: true
                text: root.title
                size: "md"
                wrapMode: Text.WordWrap
            }
            UiText {
                Layout.fillWidth: true
                visible: root.body.length > 0
                text: root.body
                tone: "muted"
                size: "sm"
                wrapMode: Text.WordWrap
            }
        }

        ChromeButton {
            Layout.alignment: Qt.AlignTop
            visible: root.dismissible
            size: "sm"
            kind: "ghost"
            iconName: "x"
            tip: qsTr("Dismiss")
            onClicked: root.dismissed()
        }
    }
}
