import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import SpanTui

// Floating find bar shared by the file viewer and the terminal: query field,
// match-case / whole-word / regex toggles (owners without them hide them),
// a match counter and previous / next / close. The owner runs the search and
// feeds back `matchCount`, `currentIndex` and `valid`; the bar only reports
// intent. Enter / Shift+Enter step to the next / previous match, Escape closes.
Item {
    id: root

    property bool valid: true
    property int matchCount: 0
    // 1-based selected match, 0 when the owner tracks no selection.
    property int currentIndex: 0
    // The count is not final (capped, or still counting); it shows "N+".
    property bool truncated: false
    property bool matchOptionsVisible: true
    readonly property alias text: searchField.text
    readonly property real renderDpr: PixelGrid.safeDpr(Screen.devicePixelRatio)

    signal queryChanged(string text, bool caseSensitive, bool wholeWord, bool regex)
    signal nextRequested(bool backwards)
    signal closeRequested()

    // Focuses the field; a non-empty `seed` replaces the query first.
    function focusSearch(seed) {
        if (seed !== undefined && seed.length > 0)
            searchField.text = seed
        searchField.forceActiveFocus()
        searchField.selectAll()
    }

    function emitQuery() {
        root.queryChanged(searchField.text, matchOptions.isChecked("case"),
                          matchOptions.isChecked("word"), matchOptions.isChecked("regex"))
    }

    // The field takes the standard input tier: a compact one clips the
    // primary-size query text.
    implicitHeight: PixelGrid.snapSize(Theme.sizeMd + Theme.spaceSm * 2, renderDpr)

    PopupSurface {
        anchors.fill: parent
        devicePixelRatio: root.renderDpr
    }

    RowLayout {
        anchors.fill: parent
        anchors.margins: Theme.spaceSm
        spacing: Theme.spaceSm

        UiField {
            id: searchField
            Layout.fillWidth: true
            Layout.minimumWidth: 88 // style-ok: search field minimum
            invalid: !root.valid
            leadingIcon: "search"
            placeholderText: qsTr("Find")
            onTextChanged: root.emitQuery()
            Keys.onReturnPressed: event => root.nextRequested(
                                      (event.modifiers & Qt.ShiftModifier) !== 0)
            Keys.onEnterPressed: event => root.nextRequested(
                                     (event.modifiers & Qt.ShiftModifier) !== 0)
            Keys.onEscapePressed: root.closeRequested()
        }

        SegmentedControl {
            id: matchOptions
            visible: root.matchOptionsVisible
            size: "sm"
            multiSelect: true
            model: [
                { key: "case", iconName: "match-case", tip: qsTr("Match case") },
                { key: "word", iconName: "match-word", tip: qsTr("Whole word") },
                { key: "regex", iconName: "match-regex", tip: qsTr("Regular expression") }
            ]
            onToggled: root.emitQuery()
        }
        UiText {
            text: root.currentIndex > 0
                  ? qsTr("%1/%2").arg(root.currentIndex)
                                 .arg(root.matchCount + (root.truncated ? "+" : ""))
                  : root.matchCount + (root.truncated ? "+" : "")
            size: "sm"
            tone: "muted"
        }
        ChromeButton {
            size: "sm"
            iconName: "chevron-down"
            rotation: 180
            kind: "ghost"
            enabled: root.matchCount > 0
            tip: qsTr("Previous match")
            onClicked: root.nextRequested(true)
        }
        ChromeButton {
            size: "sm"
            iconName: "chevron-down"
            kind: "ghost"
            enabled: root.matchCount > 0
            tip: qsTr("Next match")
            onClicked: root.nextRequested(false)
        }
        ChromeButton {
            size: "sm"
            iconName: "x"
            kind: "ghost"
            tip: qsTr("Close")
            onClicked: root.closeRequested()
        }
    }
}
