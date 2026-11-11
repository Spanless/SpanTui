import QtQuick
import SpanTui

// Shared non-modal popup exit: fast fade with a slight shrink, mirroring the
// entrance. Modal dialogs own the richer slide+pop recipe in AppDialog.
Transition {
    enabled: !Appearance.reduceMotion
    ParallelAnimation {
        NumberAnimation { property: "opacity"; to: 0; duration: Theme.durFast; easing.type: Easing.OutCubic }
        NumberAnimation { property: "scale"; to: 0.97; duration: Theme.durFast; easing.type: Easing.OutCubic }
    }
}
