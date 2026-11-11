import QtQuick
import SpanTui

// One ref decoration chip (branch / remote / tag) shared by the history rows
// and the commit hover card. Per-kind hue: local branches take the accent
// lane, remotes read blue, tags purple, the "+N" overflow chip stays muted.
// HEAD pins a solid ring of the same hue instead of the soft stroke. A local
// branch may carry `remotes` (names of remotes whose same-named branch
// points at the same commit); they trail the label in a quieter tone.
CrispSurface {
    id: badge
    required property var ref
    // Surface the chip sits on; the tint mixes from it so the chip reads
    // the same on a pane row and on a floating card.
    property color baseColor: Theme.plate

    readonly property var remotes: ref.remotes || []
    readonly property color badgeHue: ref.kind === "tag" ? Theme.gitRenamed
                                      : ref.kind === "remote" ? Theme.gitModified
                                      : ref.kind === "more" ? Theme.muted
                                      : Theme.accent

    implicitWidth: content.implicitWidth + Theme.spaceMd
    implicitHeight: Math.max(18, Theme.uiSm + 7)
    radius: Theme.radiusXs
    devicePixelRatio: PixelGrid.safeDpr(Screen.devicePixelRatio)
    fillTopColor: Theme.mix(baseColor, badgeHue, Theme.dark ? 0.26 : 0.16)
    fillMiddleColor: fillTopColor
    fillBottomColor: fillTopColor
    strokeEnabled: true
    strokeWidthPx: 1
    strokeColor: ref.head ? badgeHue
                 : Theme.mix(baseColor, badgeHue, Theme.dark ? 0.5 : 0.35)

    Row {
        id: content
        anchors.centerIn: parent
        spacing: Theme.spaceSm

        UiText {
            text: badge.ref.label
            size: "sm"
            color: badge.badgeHue
        }
        UiText {
            visible: badge.remotes.length > 0
            text: badge.remotes.join(" ")
            size: "sm"
            color: Theme.mix(badge.fillTopColor, badge.badgeHue, 0.6)
        }
    }
}
