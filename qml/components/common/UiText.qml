pragma ComponentBehavior: Bound

import QtQuick
import SpanTui

// The QML font group has no `families`: fallback chains come from a C++
// QFont. Unknown font keys (styleHint, families) abort creation.
RenderText {
    id: root

    // "sm" | "md" | "lg" | "hint" — normal UI copy uses "md"; "hint" is
    // the form-hint lane (SettingRow validation/note lines).
    property string size: "md"
    // "default" | "muted" | "accent" | "danger" | "warning" | "success"
    property string tone: "default"
    // Raises the weight to at least Medium — the selected label of a
    // segmented or toggled control. "lg" already floors at DemiBold.
    property bool emphasis: false
    property bool shimmer: false
    property color shimmerColor: Theme.dark ? "#ffffff" : "#a7f3d0" // style-ok: shimmer beam tint
    // A decorative loop, not a transition: no dur* tier.
    readonly property int shimmerSweepMs: 1300
    readonly property int shimmerGapMs: 1100

    readonly property int pixelSize: {
        // dependency: typography changes re-evaluate the size tier
        const _revision = Appearance.fontRevision
        return size === "sm" ? Theme.uiSm
             : size === "hint" ? Theme.uiHint
             : Theme.uiMd
    }

    font: {
        const f = root.size === "sm"
                  ? Appearance.uiAuxFont(root.pixelSize, Appearance.fontRevision)
                  : root.size === "hint"
                    ? Appearance.formHintFont(root.pixelSize, Appearance.fontRevision)
                    : Appearance.uiFont(root.pixelSize, Appearance.fontRevision)
        const floor = root.size === "lg" ? Font.DemiBold
                    : root.emphasis ? Font.Medium : 0
        if (f.weight < floor)
            f.weight = floor
        return f
    }

    color: tone === "muted" ? Theme.muted
         : tone === "accent" ? Theme.accent
         : tone === "danger" ? Theme.danger
         : tone === "warning" ? Theme.warning
         : tone === "success" ? Theme.success
         : Theme.text

    Loader {
        id: shimmerLoader
        anchors.fill: parent
        active: root.shimmer && root.visible && !Appearance.reduceMotion
        sourceComponent: Item {
            id: shimmerHost
            anchors.fill: parent
            clip: true

            readonly property real beamWidth: Math.max(22, Math.min(54, root.width * 0.30))

            Item {
                id: sheenContainer
                width: shimmerHost.beamWidth
                height: parent.height
                clip: true
                x: -width

                RenderText {
                    id: highlightText
                    x: -sheenContainer.x
                    y: 0
                    width: root.width
                    height: root.height
                    text: root.text
                    font: root.font
                    color: root.shimmerColor
                    elide: root.elide
                    horizontalAlignment: root.horizontalAlignment
                    verticalAlignment: root.verticalAlignment
                }
            }

            // The gap is a Timer, not a PauseAnimation: any running animation,
            // a pause included, keeps the whole window rendering every vsync.
            readonly property bool sweeping: visible && root.width > 0
            onSweepingChanged: {
                if (sweeping) {
                    shimmerSweep.restart()
                } else {
                    shimmerSweep.stop()
                    shimmerGap.stop()
                }
            }
            Component.onCompleted: if (sweeping) shimmerSweep.start()

            NumberAnimation {
                id: shimmerSweep
                target: sheenContainer
                property: "x"
                from: -shimmerHost.beamWidth
                to: root.width
                duration: root.shimmerSweepMs
                easing.type: Easing.InOutQuad
                // Not emitted by stop(), so a stopped sweep never rearms.
                onFinished: shimmerGap.start()
            }
            Timer {
                id: shimmerGap
                interval: root.shimmerGapMs
                onTriggered: shimmerSweep.start()
            }
        }
    }
}
