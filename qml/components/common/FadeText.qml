import QtQuick
import SpanTui

// Display-side overflow treatment for length-limited text (paths, long
// names): the head always shows and the clipped tail dissolves into the
// surface instead of ending in an ellipsis. Set `fadeColor` to the surface
// the text sits on — the fade is a gradient overlay, not real alpha, so it
// only reads correctly on a uniform background.
// Sibling of UiField's `fadeOverflow` (the input-side variant of the same
// recipe); pick this one for read-only display.
Item {
    id: root

    property alias text: label.text
    // "sm" | "md" | "lg" — same tiers as UiText.
    property string size: "md"
    // "default" | "muted" | "accent" | "danger" | "warning" | "success".
    property string tone: "default"
    // Explicit label color; overrides `tone` (e.g. accentText on a fill).
    property alias color: label.color
    property color fadeColor: Theme.surfaceRaised
    property real fadeWidth: Theme.sizeMd

    implicitWidth: label.implicitWidth
    implicitHeight: label.implicitHeight

    UiText {
        id: label
        width: parent.width
        height: parent.height
        size: root.size
        tone: root.tone
        elide: Text.ElideNone
        clip: true
        verticalAlignment: Text.AlignVCenter
    }

    Item {
        visible: label.contentWidth > label.width
        anchors.right: parent.right
        anchors.top: parent.top
        anchors.bottom: parent.bottom
        width: Math.min(root.fadeWidth, parent.width)

        // Rectangle gradients run top→bottom; rotate to get left→right.
        Rectangle {
            anchors.centerIn: parent
            width: parent.height
            height: parent.width
            rotation: -90
            gradient: Gradient {
                GradientStop { position: 0.0; color: "transparent" }
                GradientStop { position: 1.0; color: root.fadeColor }
            }
        }
    }
}
