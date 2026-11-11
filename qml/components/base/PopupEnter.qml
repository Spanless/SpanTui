import QtQuick
import SpanTui

// Shared non-modal popup entrance (tooltips, menus): the fade lands fast while
// the scale settles a beat longer on the emphasized curve, so the panel reads
// as arriving rather than blinking in. Modal dialogs own the richer slide+pop
// recipe in AppDialog.
Transition {
    enabled: !Appearance.reduceMotion
    ParallelAnimation {
        NumberAnimation { property: "opacity"; from: 0; to: 1; duration: Theme.durFast; easing.type: Easing.OutCubic }
        NumberAnimation { property: "scale"; from: 0.94; to: 1; duration: Theme.durMid; easing.type: Easing.BezierSpline; easing.bezierCurve: Theme.easeEmphasized }
    }
}
