import QtQuick
import QtQuick.Controls
import SpanTui

// Single-line input. `leading`/`trailing` host items inside the frame.
// `fadeOverflow` (paths): unfocused, the text shows its head and the tail
// fades out.
TextField {
    id: root

    // "sm" | "md" — control height tier.
    property string size: "md"
    property bool invalid: false
    // "ui" (interface font chain) | "mono" (editor font chain — hex, paths).
    property string role: "ui"
    // "sm" | "md" — text size tier; field content is primary text so md is
    // the default, sm stays for dense tool strips.
    property string textSize: "md"
    property string leadingIcon: ""
    // Head-anchored idle display + tail fade for over-long content (paths).
    property bool fadeOverflow: false
    property alias leading: leadingHost.data
    property alias trailing: trailingHost.data

    readonly property int textPx: textSize === "md" ? Theme.uiMd : Theme.uiSm
    readonly property color fillColor: activeFocus ? Theme.raisedFill
                                                   : Theme.surfaceInset
    readonly property bool overflowing:
        contentWidth > width - leftPadding - rightPadding

    implicitWidth: Theme.fieldMd
    // The tier is the floor; a larger font lane grows the field instead of
    // clipping its line box.
    implicitHeight: Theme.fitText(size === "sm" ? Theme.sizeSm : Theme.sizeMd, lineMetrics.height)
    // The text is centered by verticalAlignment. The Basic style's 6px
    // top/bottom padding left a sm field 10px for a ~17px line and cut the
    // glyphs.
    topPadding: 0
    bottomPadding: 0
    renderType: Text.QtRendering
    color: Theme.text
    placeholderTextColor: Theme.muted
    selectionColor: Theme.textSelection
    selectedTextColor: Theme.text
    selectByMouse: true
    font: role === "mono" ? Appearance.editorFont(textPx, Appearance.fontRevision)
          : textSize === "sm" ? Appearance.uiAuxFont(textPx, Appearance.fontRevision)
                              : Appearance.uiFont(textPx, Appearance.fontRevision)
    leftPadding: leadIcon.visible ? Theme.spaceMd + Theme.iconSm + Theme.spaceSm
                 : leadingHost.visible ? Theme.spaceMd + leadingHost.implicitWidth + Theme.spaceSm
                 : Theme.spaceMd
    // The text clips spaceSm short of the trailing actions — the same edge
    // the tail fade ends on, so no unfaded sliver shows between them.
    rightPadding: trailingHost.visible ? Theme.spaceMd + trailingHost.implicitWidth
                                         + Theme.spaceSm
                                       : Theme.spaceMd
    verticalAlignment: TextInput.AlignVCenter

    // TextField parks the caret at the end of assigned text. Deferred: the
    // control's own caret bookkeeping after textChanged would overwrite it.
    function anchorHead() {
        if (!fadeOverflow || activeFocus)
            return
        Qt.callLater(function() {
            if (!activeFocus)
                cursorPosition = 0
        })
    }
    onTextChanged: anchorHead()
    onActiveFocusChanged: anchorHead()

    FontMetrics {
        id: lineMetrics
        font: root.font
    }

    Icon {
        id: leadIcon
        visible: root.leadingIcon.length > 0
        anchors.left: parent.left
        anchors.leftMargin: Theme.spaceMd
        anchors.verticalCenter: parent.verticalCenter
        name: root.leadingIcon
        size: Theme.iconSm
        color: Theme.muted
    }

    Row {
        id: leadingHost
        visible: !leadIcon.visible && children.length > 0
        anchors.left: parent.left
        anchors.leftMargin: Theme.spaceMd
        anchors.verticalCenter: parent.verticalCenter
        spacing: Theme.spaceSm
    }

    Row {
        id: trailingHost
        visible: children.length > 0
        anchors.right: parent.right
        anchors.rightMargin: Theme.spaceMd
        anchors.verticalCenter: parent.verticalCenter
        spacing: Theme.spaceSm
    }

    // Tail fade: a horizontal gradient over the clipped end of the text,
    // painted in the field's own fill color so the text appears to dissolve.
    // Hidden while editing — the caret needs the real tail.
    Item {
        id: tailFade
        visible: root.fadeOverflow && root.overflowing && !root.activeFocus
        anchors.right: trailingHost.visible ? trailingHost.left : parent.right
        anchors.rightMargin: trailingHost.visible ? Theme.spaceSm
                                                  : Theme.spaceMd
        anchors.top: parent.top
        anchors.bottom: parent.bottom
        anchors.topMargin: 1    // style-ok: clear the frame hairline
        anchors.bottomMargin: 1 // style-ok: clear the frame hairline
        width: Theme.sizeMd

        // Rectangle gradients run top→bottom; rotate to get left→right.
        Rectangle {
            anchors.centerIn: parent
            width: parent.height
            height: parent.width
            rotation: -90
            gradient: Gradient {
                GradientStop { position: 0.0; color: "transparent" }
                GradientStop { position: 1.0; color: root.fillColor }
            }
        }
    }

    opacity: enabled ? 1 : Theme.opacityDisabled
    Behavior on opacity {
        enabled: !Appearance.reduceMotion
        NumberAnimation { duration: Theme.durFast; easing.type: Easing.OutCubic }
    }

    background: CrispSurface {
        radius: Theme.radiusSm
        fillTopColor: root.fillColor
        fillMiddleColor: fillTopColor
        fillBottomColor: fillTopColor
        focusRing: root.activeFocus && !root.invalid
        strokeEnabled: focusRing || root.invalid
        strokeColor: root.invalid ? Theme.danger : Theme.accent
    }
}
