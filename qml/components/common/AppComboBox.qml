pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import SpanTui

// The only dropdown in product QML. `activated(index)` carries the index in
// the source model, not the filtered view. Custom delegates should build on
// ComboOptionRow; otherwise they must call pickIndex(index) on click and
// size to root.listWidth.
Item {
    id: root

    property var model: []
    property int currentIndex: -1
    property bool filterable: true
    // The face is always an input; the list only offers presets. Enter and
    // blur commit through parseInput.
    property bool editable: false
    property var currentValue: undefined
    property var valueText: function(value) { return String(value) }
    // fn(text) → value, or undefined/null to reject and roll back.
    property var parseInput: null
    property string suffixText: ""
    property string displayText: editable && currentValue !== undefined
                                 ? valueText(currentValue)
                                 : (currentIndex >= 0 && currentIndex < count
                                    ? itemTextForIndex(currentIndex,
                                                       model[currentIndex])
                                    : "")
    property string placeholderText: ""
    property string leadingIcon: ""
    property string leadingProgramIcon: ""
    property bool warning: false
    property int maximumVisibleRows: 10
    // Index into the filtered view.
    property int highlightedIndex: -1
    property var itemTextForIndex: function(index, modelData) {
        return String(modelData)
    }
    // fn(index, modelData) → program path whose icon leads each option.
    property var itemProgramIconForIndex: null
    property Component delegate: defaultRow

    readonly property int count: model.length
    readonly property alias popup: comboPopup
    // The popup has no right padding so the scrollbar reaches its edge; rows
    // keep the spaceSm inset themselves.
    readonly property real listWidth: Math.max(
        0, comboPopup.availableWidth - Theme.spaceSm)

    signal activated(int index)
    signal submitted(var value)

    readonly property real renderDpr: PixelGrid.safeDpr(Screen.devicePixelRatio)

    implicitWidth: Theme.fieldMd
    implicitHeight: Theme.sizeMd
    opacity: enabled ? 1 : Theme.opacityDisabled

    Accessible.role: Accessible.ComboBox
    Accessible.name: root.displayText

    property string filterText: ""
    readonly property var _filtered: {
        var out = []
        var src = []
        var needle = root.filterText.toLowerCase()
        for (var i = 0; i < root.count; ++i) {
            if (needle.length === 0
                || root.itemTextForIndex(i, root.model[i])
                          .toLowerCase().indexOf(needle) >= 0) {
                out.push(root.model[i])
                src.push(i)
            }
        }
        return { "items": out, "source": src }
    }
    readonly property var filteredModel: _filtered.items
    readonly property var filteredSourceIndex: _filtered.source

    function sourceIndexFor(viewIndex) {
        return viewIndex >= 0 && viewIndex < filteredSourceIndex.length
               ? filteredSourceIndex[viewIndex] : -1
    }

    // Set when a pick or Esc already decided the value, so closing does not
    // also commit the field text.
    property bool _suppressCloseCommit: false

    function commitText(text) {
        const parsed = root.parseInput ? root.parseInput(text) : text
        if (parsed === undefined || parsed === null) {
            edit.text = root.displayText
            return false
        }
        root.submitted(parsed)
        return true
    }

    function pickIndex(viewIndex) {
        var src = sourceIndexFor(viewIndex)
        if (src < 0)
            return
        root._suppressCloseCommit = true
        root.activated(src)
        // Written now: if focus survives the close, the closed-face Binding
        // (active only while unfocused) never shows the pick.
        if (root.editable)
            edit.text = root.valueText(root.model[src])
        comboPopup.close()
    }

    function openPopup() {
        comboPopup.openBelow(face)
    }

    Item {
        id: face
        anchors.fill: parent
        // PopupToggleGuard.sourceHeld reads `down` off the anchor.
        readonly property bool down: faceMouse.pressed

        CrispSurface {
            anchors.fill: parent
            devicePixelRatio: root.renderDpr
            radius: Theme.radiusSm
            elevation: root.editable ? "none" : "raised"
            focusRing: edit.activeFocus && !root.warning
            fillTopColor: root.editable
                          ? (edit.activeFocus ? Theme.raisedFill : Theme.surfaceInset)
                          : (faceMouse.pressed ? Theme.raisedFillPressed
                             : faceMouse.containsMouse ? Theme.raisedFillHover
                             : Theme.raisedFillTop)
            fillMiddleColor: root.editable
                             ? fillTopColor
                             : (faceMouse.pressed ? Theme.raisedFillPressed
                                : faceMouse.containsMouse ? Theme.raisedFillHover
                                : Theme.raisedFill)
            strokeEnabled: focusRing || root.warning || !root.editable
            strokeWidthPx: root.warning ? 1.5 : 1
            strokeColor: root.warning ? Theme.warning
                         : focusRing ? Theme.accent : Theme.buttonRim
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

        ProgramIcon {
            id: leadProgram
            visible: root.leadingIcon.length === 0 && root.leadingProgramIcon.length > 0
            anchors.left: parent.left
            anchors.leftMargin: Theme.spaceMd
            anchors.verticalCenter: parent.verticalCenter
            path: root.leadingProgramIcon
            size: Theme.iconSm
        }

        TextInput {
            id: edit
            anchors.left: leadIcon.visible ? leadIcon.right
                          : (leadProgram.visible ? leadProgram.right : parent.left)
            anchors.leftMargin: Theme.spaceMd
            anchors.right: suffix.visible ? suffixDivider.left
                          : (chevron.visible ? chevron.left : parent.right)
            anchors.rightMargin: Theme.spaceMd
            anchors.top: parent.top
            anchors.bottom: parent.bottom
            readOnly: root.editable ? false
                      : (!comboPopup.opened || !root.filterable)
            clip: true
            selectByMouse: root.filterable || root.editable
            verticalAlignment: TextInput.AlignVCenter
            // Read-only text is drawn by faceLabel: TextInput centers a CJK
            // fallback line ~2px higher than Text with the same font.
            color: readOnly ? "transparent" : Theme.text
            selectionColor: Theme.textSelection
            selectedTextColor: Theme.text
            font: Appearance.uiFont(Theme.uiMd, Appearance.fontRevision)

            HoverHandler {
                enabled: !edit.readOnly
                cursorShape: Qt.IBeamCursor
            }

            // Standalone Binding: the `on <prop>` form trips the raw-text style rule.
            Binding {
                target: edit
                property: "text"
                value: root.displayText
                when: root.editable ? !edit.activeFocus
                                    : (!comboPopup.opened || !root.filterable)
                restoreMode: Binding.RestoreNone
            }

            // Editable: focus opens the presets; blur while open closes them
            // (the commit runs in onAboutToHide); blur while closed commits.
            onActiveFocusChanged: {
                if (!root.editable)
                    return
                if (activeFocus) {
                    if (!comboPopup.opened)
                        root.openPopup()
                } else if (comboPopup.opened) {
                    comboPopup.close()
                } else if (text !== root.displayText) {
                    root.commitText(text)
                }
            }

            // Set by ↑↓, cleared by typing: Enter then picks the highlight
            // instead of committing the typed text.
            property bool highlightPicked: false

            function commit() {
                if (highlightPicked && root.highlightedIndex >= 0
                        && popupList.rowCount > 0) {
                    root.pickIndex(root.highlightedIndex)
                    return
                }
                if (!root.editable) {
                    if (popupList.rowCount > 0 && root.highlightedIndex >= 0)
                        root.pickIndex(root.highlightedIndex)
                    return
                }
                if (root.commitText(text)) {
                    root._suppressCloseCommit = true
                    comboPopup.close()
                }
            }

            onTextChanged: {
                if (comboPopup.opened && (root.filterable || root.editable)) {
                    root.filterText = text
                    root.highlightedIndex = popupList.rowCount > 0 ? 0 : -1
                    highlightPicked = false
                } else {
                    // An assignment parks the caret at the end and scrolls
                    // the text; keep the head visible.
                    const self = edit
                    Qt.callLater(function() {
                        if (!comboPopup.opened)
                            self.cursorPosition = 0
                    })
                }
            }
            Keys.onUpPressed: {
                root.highlightedIndex = Math.max(0, root.highlightedIndex - 1)
                highlightPicked = true
            }
            Keys.onDownPressed: {
                root.highlightedIndex = Math.min(popupList.rowCount - 1,
                                                 root.highlightedIndex + 1)
                highlightPicked = true
            }
            Keys.onReturnPressed: edit.commit()
            Keys.onEnterPressed: edit.commit()
            Keys.onEscapePressed: {
                root._suppressCloseCommit = true
                edit.text = root.displayText
                comboPopup.close()
            }
        }

        PixelLine {
            id: suffixDivider
            visible: root.editable && root.suffixText.length > 0
            anchors.right: suffix.left
            anchors.rightMargin: Theme.spaceSm
            anchors.top: parent.top
            anchors.bottom: parent.bottom
            anchors.topMargin: Theme.spaceSm
            anchors.bottomMargin: Theme.spaceSm
            vertical: true
            lineColor: Theme.border
        }

        UiText {
            id: suffix
            visible: root.editable && root.suffixText.length > 0
            anchors.right: chevron.visible ? chevron.left : parent.right
            anchors.rightMargin: Theme.spaceMd
            anchors.verticalCenter: parent.verticalCenter
            text: root.suffixText
            tone: "muted"
            size: "md"
        }

        UiText {
            id: faceLabel
            anchors.fill: edit
            verticalAlignment: Text.AlignVCenter
            visible: edit.readOnly && edit.text.length > 0
            text: edit.text
            size: "md"
            elide: Text.ElideRight
        }

        // Placeholder. Hidden during IME composition, which would draw over it.
        UiText {
            anchors.fill: edit
            verticalAlignment: Text.AlignVCenter
            visible: edit.text.length === 0 && !edit.inputMethodComposing
            // `visible`, not `opened`: opening clears the face text, and
            // `opened` turns true only after the enter transition.
            text: comboPopup.visible && (root.filterable || root.editable)
                  ? root.displayText : root.placeholderText
            tone: "muted"
            size: "md"
            elide: Text.ElideRight
        }

        Icon {
            id: chevron
            visible: !root.editable
            anchors.right: parent.right
            anchors.rightMargin: Theme.spaceMd
            anchors.verticalCenter: parent.verticalCenter
            name: "chevrons-up-down"
            size: Theme.iconSm
            color: root.warning ? Theme.warning
                   : (edit.activeFocus || faceMouse.containsMouse
                      ? Theme.text : Theme.muted)
        }

        // Disabled while open: a press on the face is then an outside press
        // that PopupToggleGuard turns into a single close, never a reopen.
        MouseArea {
            id: faceMouse
            anchors.fill: parent
            enabled: root.enabled && !comboPopup.opened && !root.editable
            hoverEnabled: true
            cursorShape: Qt.PointingHandCursor
            onClicked: root.openPopup()
        }
    }

    AppPopup {
        id: comboPopup
        autoSizeToContent: false
        width: root.width
        padding: Theme.spaceSm
        rightPadding: 0
        edgeMargin: Theme.spaceSm
        height: popupList.implicitHeight + topPadding + bottomPadding

        onOpening: {
            root.filterText = ""
            root.highlightedIndex = root.filteredSourceIndex.indexOf(
                                        root.currentIndex)
            // A picker without a filter keeps its text: its face binding
            // never lets go, so nothing would restore a cleared value.
            if (root.editable)
                edit.text = root.displayText
            else if (root.filterable)
                edit.text = ""
        }

        // Not at click time: AppPopup takes focus on open, and the caret
        // would never show.
        onOpened: {
            if (root.filterable || root.editable) {
                edit.forceActiveFocus()
                if (root.editable)
                    edit.selectAll()
            }
        }

        onAboutToHide: {
            if (root.editable && !root._suppressCloseCommit
                    && edit.text !== root.displayText)
                root.commitText(edit.text)
            root._suppressCloseCommit = false
        }

        onPopupClosed: edit.focus = false

        contentItem: ComboPopupList {
            id: popupList
            combo: root
        }
    }

    Component {
        id: defaultRow
        ComboOptionRow { combo: root }
    }
}
