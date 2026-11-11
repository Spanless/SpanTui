import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import SpanTui

// Single clickable control for the whole app (icon and/or text).
// Kinds map onto the elevation recipes (design-system.md §5):
//   primary   → raised accent fill with a top sheen, Medium accentText label
//   secondary → raised neutral thumb (the default push button)
//   danger    → the secondary thumb with a danger label (destructive
//               confirms sit next to secondary buttons and must match them)
//   ghost     → flat until hovered; toolbars and dense clusters
//   selected  → a toggled-on ghost: sunken tint, Medium body-color label
//   close     → window-chrome close: flat, danger fill + dangerText on hover
// Every clickable kind has a hover and a pressed tone (raised faces take the
// scheme state layers, solid fills the Theme.solidFill recipe). Keyboard
// focus only adds the shared focus ring — it never changes fill or label.
// Icon-only controls use either the standard or compact square token.
Button {
    id: control

    // "secondary" | "primary" | "danger" | "ghost" | "selected" | "close"
    property string kind: "secondary"
    property string iconName: ""
    property string tip: ""
    // "sm" | "md" — "sm" shrinks the click target and glyph for dense
    // headers, rails, and inline tool clusters.
    property string size: "md"
    readonly property bool compact: size === "sm"
    property real iconSize: compact ? Theme.iconSm : Theme.iconMd

    readonly property bool iconOnly: iconName.length > 0 && text.length === 0
    readonly property real renderDpr: PixelGrid.safeDpr(Screen.devicePixelRatio)
    readonly property real controlSize: PixelGrid.snapSize(
                                                compact ? Theme.sizeSm
                                                        : Theme.sizeMd,
                                                renderDpr)
    readonly property bool engaged: enabled && (hovered || down)
    readonly property bool raised: kind === "primary" || kind === "secondary"
                                   || kind === "danger"

    property color surfaceColor: {
        if (kind === "primary") {
            if (down)
                return Theme.accentPressed
            if (hovered)
                return Theme.accentHover
            return Theme.accent
        }
        if (kind === "secondary" || kind === "danger")
            return down ? Theme.raisedFillPressed
                        : hovered ? Theme.raisedFillHover : Theme.raisedFill
        if (kind === "selected")
            return down ? Theme.pressedOnInset : Theme.surfaceInset
        if (kind === "close") {
            if (down)
                return Theme.dangerPressed
            if (hovered)
                return Theme.danger
            return "transparent"
        }
        // ghost
        if (down)
            return Theme.pressed
        if (hovered)
            return Theme.hover
        return "transparent"
    }
    // Raised kinds light the top edge at rest; hover and press are flat tones.
    readonly property color surfaceTopColor: engaged ? surfaceColor
                                             : kind === "primary" ? Theme.accentTop
                                             : kind === "secondary" || kind === "danger"
                                               ? Theme.raisedFillTop
                                             : surfaceColor
    readonly property bool surfaceFillEnabled: surfaceColor.a > 0

    readonly property color fg: {
        if (kind === "primary")
            return Theme.accentText
        if (kind === "danger")
            return Theme.danger
        if (kind === "close")
            return hovered || down ? Theme.dangerText : Theme.muted
        if (kind === "secondary" || kind === "selected")
            return Theme.text
        return engaged ? Theme.text : Theme.muted
    }

    implicitHeight: controlSize
    // Labeled buttons breathe wider than the row padding; compact ones keep
    // the tighter tool-strip inset.
    readonly property real labelPad: compact ? Theme.spaceMd : Theme.spaceMd + Theme.spaceSm
    implicitWidth: iconOnly ? controlSize
                            : Math.max(controlSize, Math.ceil(labelRow.implicitWidth) + labelPad * 2)

    // Absolute size lock (also via Layout.* so RowLayout/ColumnLayout cannot stretch).
    height: controlSize
    width: iconOnly ? controlSize : implicitWidth

    Layout.fillWidth: false
    Layout.fillHeight: false
    Layout.preferredWidth: width
    Layout.preferredHeight: controlSize
    Layout.maximumWidth: iconOnly ? controlSize : 100000
    Layout.maximumHeight: controlSize
    Layout.minimumWidth: iconOnly ? controlSize : implicitWidth
    Layout.minimumHeight: controlSize
    Layout.alignment: Qt.AlignVCenter

    padding: 0
    leftPadding: 0
    rightPadding: 0
    topPadding: 0
    bottomPadding: 0
    focusPolicy: Qt.TabFocus

    opacity: control.enabled ? 1 : Theme.opacityDisabled
    Behavior on opacity { NumberAnimation { duration: Theme.durFast; easing.type: Easing.OutCubic } }

    // Tactile press: the whole face sinks while held and springs back on
    // release. Fill and label tones still switch instantly — only geometry
    // moves. Window-chrome close stays put (it sits flush in the title bar).
    scale: down && kind !== "close" ? Theme.pressScale : 1
    Behavior on scale {
        enabled: !Appearance.reduceMotion
        NumberAnimation {
            duration: control.down ? Theme.durFast : Theme.durMid
            easing.type: control.down ? Easing.OutCubic : Easing.OutBack
            easing.overshoot: 2.4
        }
    }

    Accessible.role: Accessible.Button
    Accessible.name: {
        if (control.tip.length > 0)
            return control.tip
        if (control.text.length > 0)
            return control.text
        if (control.iconName.length > 0)
            return control.iconName
        return qsTr("Button")
    }
    Accessible.onPressAction: control.clicked()

    // contentItem sized by Button to our width×height; only center children.
    contentItem: Item {
        Row {
            id: labelRow
            anchors.centerIn: parent
            spacing: Theme.spaceMd

            Icon {
                visible: control.iconName.length > 0
                anchors.verticalCenter: parent.verticalCenter
                name: control.iconName
                size: control.iconSize
                color: control.fg
                hovered: control.hovered
                pressed: control.down
                active: control.kind === "selected" || control.checked
                // Primary and close paint a solid accent/danger fill.
                tinted: control.kind !== "primary" && control.kind !== "close"
            }

            UiText {
                visible: control.text.length > 0
                anchors.verticalCenter: parent.verticalCenter
                text: control.text
                color: control.fg
                emphasis: control.kind === "primary" || control.kind === "selected"
                // The label is the control's identity — primary tier regardless
                // of the compact height class.
                size: "md"
            }
        }
    }

    background: CrispSurface {
        devicePixelRatio: control.renderDpr
        radius: Theme.radiusSm
        elevation: control.raised ? "raised" : "none"
        focusRing: control.visualFocus
        strokeColor: control.visualFocus ? Theme.accent : Theme.buttonRim
        fillEnabled: control.surfaceFillEnabled
        fillTopColor: control.surfaceTopColor
        fillMiddleColor: control.surfaceColor
        fillBottomColor: control.surfaceColor
    }

    Tip {
        text: control.tip
        visible: control.tip.length > 0 && control.hovered
    }
}
