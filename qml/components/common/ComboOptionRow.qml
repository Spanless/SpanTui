import QtQuick
import QtQuick.Controls
import SpanTui

// One option row of an AppComboBox popup. Hover runs through the list's
// glide pill (a solid accent fill); the row paints only press /
// keyboard-highlight fills itself, in the same accent. `onFill` marks the
// row under any of them: its label turns accentText and the FadeText
// dissolves into the accent. The committed value carries a leading check
// mark (`current`) — the highlight only tracks pointer/keyboard position,
// so it never doubles as the current-value marker. Extra
// content (e.g. a font sample) goes into the trailing cell, sized by
// `trailingWidth`. Clicking commits through `combo.pickIndex(index)`.
ItemDelegate {
    id: row

    required property int index
    required property var modelData
    // The owning AppComboBox: text, highlight and pick all route through it.
    required property Item combo
    property real trailingWidth: 0
    default property alias trailing: trailingCell.data

    readonly property bool glideRow: true
    readonly property bool glideManaged: Boolean(ListView.view && ListView.view.glideHover)
    readonly property bool glideActive: Boolean(ListView.view && ListView.view.glideActive)
    readonly property bool keyHighlighted: highlighted && !glideActive
    // View index → source index: the list may be filtered.
    readonly property bool current: combo.currentIndex >= 0
                                    && combo.filteredSourceIndex[index] === combo.currentIndex
    readonly property bool onFill: down || keyHighlighted
                                   || (glideManaged
                                       ? ListView.view.glideContext.currentRow === row
                                       : hovered)

    width: combo.listWidth
    height: Theme.sizeMd
    highlighted: combo.highlightedIndex === index
    hoverEnabled: true
    // Basic ItemDelegate reserves style padding around contentItem; the row
    // owns its spacing, and that padding would clip glyph ascenders.
    padding: 0
    onClicked: combo.pickIndex(index)

    contentItem: Item {
        clip: true

        // Every row reserves the check column so labels stay aligned.
        Icon {
            id: checkMark
            anchors.left: parent.left
            anchors.leftMargin: Theme.spaceSm
            anchors.verticalCenter: parent.verticalCenter
            name: "check"
            size: Theme.iconSm
            color: row.onFill ? Theme.accentText : Theme.text
            opacity: row.current ? 1 : 0
        }

        ProgramIcon {
            id: programIcon
            visible: path.length > 0
            anchors.left: checkMark.right
            anchors.leftMargin: Theme.spaceSm
            anchors.verticalCenter: parent.verticalCenter
            path: row.combo.itemProgramIconForIndex
                  ? row.combo.itemProgramIconForIndex(row.index, row.modelData) || ""
                  : ""
            size: Theme.iconSm
        }

        // Long entries dissolve at the tail, never ellipsis. fadeColor must
        // track the row's own state background.
        FadeText {
            anchors.left: parent.left
            anchors.right: trailingCell.visible ? trailingCell.left : parent.right
            anchors.top: parent.top
            anchors.bottom: parent.bottom
            anchors.leftMargin: Theme.spaceSm * 2 + Theme.iconSm
                                + (programIcon.visible ? Theme.iconSm + Theme.spaceSm : 0)
            anchors.rightMargin: Theme.spaceMd
            text: row.combo.itemTextForIndex(row.index, row.modelData)
            size: "md"
            color: row.onFill ? Theme.accentText : Theme.text
            fadeColor: row.down ? Theme.accentPressed
                       : row.onFill ? Theme.accent
                       : Theme.overlaySurface
        }

        Item {
            id: trailingCell
            visible: row.trailingWidth > 0
            anchors.right: parent.right
            anchors.top: parent.top
            anchors.bottom: parent.bottom
            anchors.rightMargin: Theme.spaceMd
            width: row.trailingWidth
            clip: true
        }
    }

    background: CrispSurface {
        devicePixelRatio: row.combo.renderDpr
        radius: Theme.radiusSm
        fillEnabled: row.down || (row.hovered && !row.glideManaged) || row.keyHighlighted
        fillTopColor: row.down ? Theme.accentPressed : Theme.accent
        fillMiddleColor: fillTopColor
        fillBottomColor: fillTopColor
        strokeEnabled: false
        shadowEnabled: false
    }
}
