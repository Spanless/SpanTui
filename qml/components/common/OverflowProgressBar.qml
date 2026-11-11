pragma ComponentBehavior: Bound

import QtQuick
import SpanTui

// Capsule track with a traveling sheen. Unknown or completed progress uses
// the full-width overflow sweep so a stuck network op still reads as live.
Item {
    id: root

    property real progress: -1
    property color fillColor: Theme.accent
    property int barWidth: 56
    property int barHeight: 6
    property bool motionActive: true
    property bool indeterminate: progress < 0
    readonly property real renderDpr: PixelGrid.safeDpr(Screen.devicePixelRatio)
    readonly property bool reduceMotion: Appearance.reduceMotion
    readonly property bool overflowActive: indeterminate
    readonly property real fillRatio: indeterminate
                                      ? 1
                                      : Math.max(0, Math.min(1, progress))
    readonly property color fillHi: Theme.composite(Theme.mix(root.fillColor, "#ffffff", Theme.dark ? 0.18 : 0.10), fillColor) // style-ok: sheen highlight tint weight
    readonly property int snappedBarW: PixelGrid.snapSize(barWidth, renderDpr)
    readonly property int snappedBarH: PixelGrid.snapSize(barHeight, renderDpr)
    readonly property int snappedFillW: {
        if (fillRatio <= 0)
            return 0
        const raw = snappedBarW * fillRatio
        const floor = Math.min(snappedBarH, snappedBarW)
        return PixelGrid.snapSize(Math.max(floor, raw), renderDpr)
    }
    readonly property int sheenW: PixelGrid.snapSize(
        Math.max(snappedBarH * 3, Math.round(snappedBarW * 0.42)), renderDpr)

    implicitWidth: snappedBarW
    implicitHeight: snappedBarH
    width: implicitWidth
    height: implicitHeight

    CrispSurface {
        anchors.fill: parent
        devicePixelRatio: root.renderDpr
        radius: height / 2
        fillTopColor: Theme.surfaceInset
        fillMiddleColor: Theme.surfaceInset
        fillBottomColor: Theme.surfaceInset
        strokeEnabled: false
    }

    Item {
        id: fillClip
        width: root.snappedFillW
        height: parent.height
        clip: true
        visible: width > 0
        // Determinate steps glide to the new fill instead of jumping; the
        // indeterminate sweep is always full width and needs no easing.
        Behavior on width {
            enabled: !root.reduceMotion && !root.indeterminate
            NumberAnimation {
                duration: Theme.durMid
                easing.type: Easing.BezierSpline
                easing.bezierCurve: Theme.easeEmphasized
            }
        }

        CrispSurface {
            width: fillClip.width
            height: fillClip.height
            devicePixelRatio: root.renderDpr
            radius: height / 2
            fillTopColor: root.fillHi
            fillMiddleColor: root.fillColor
            fillBottomColor: root.fillColor
            strokeEnabled: false
        }

        Rectangle {
            id: sheen
            width: root.sheenW
            height: parent.height
            visible: !root.reduceMotion && fillClip.width > 0
            radius: height / 2
            gradient: Gradient {
                orientation: Gradient.Horizontal
                GradientStop { position: 0.0; color: "transparent" }
                GradientStop {
                    position: 0.45
                    color: Theme.composite(Theme.mix(root.fillColor, "#ffffff", Theme.dark ? 0.18 : 0.10), root.fillColor) // style-ok: sheen highlight tint weight
                }
                GradientStop { position: 1.0; color: "transparent" }
            }
        }
    }

    // Sheen cadence: active overflow sweeps faster with a tighter pause.
    readonly property int sheenSweepActiveMs: 1100
    readonly property int sheenSweepIdleMs: 1400
    readonly property int sheenPauseActiveMs: 80
    readonly property int sheenPauseIdleMs: 160

    SequentialAnimation {
        id: sheenSweep
        running: sheen.visible && fillClip.width > 0 && root.motionActive
        loops: Animation.Infinite
        NumberAnimation {
            target: sheen
            property: "x"
            from: -root.sheenW
            to: fillClip.width + (root.overflowActive ? root.sheenW * 0.15 : 0)
            duration: root.overflowActive ? root.sheenSweepActiveMs
                                          : root.sheenSweepIdleMs
            easing.type: root.overflowActive ? Easing.InOutSine : Easing.Linear
        }
        PauseAnimation {
            duration: root.overflowActive ? root.sheenPauseActiveMs
                                          : root.sheenPauseIdleMs
        }
    }
}
