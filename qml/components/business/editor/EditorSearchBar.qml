import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import SpanTui

// File viewer overlay: the shared FindBar, or a go-to-line field.
Item {
    id: root

    property bool searchVisible: false
    property bool gotoVisible: false
    property bool searchValid: true
    property int matchCount: 0
    property int lineCount: 1
    property int cursorLine: 1
    readonly property real renderDpr: PixelGrid.safeDpr(Screen.devicePixelRatio)
    readonly property bool barOpen: searchVisible || gotoVisible

    signal queryChanged(string text, bool caseSensitive, bool wholeWord, bool regex)
    signal nextRequested(bool backwards)
    signal goToLineRequested(int line)
    signal closeRequested()

    function focusSearch() {
        findBar.focusSearch()
    }

    function focusGoto() {
        gotoField.text = String(root.cursorLine)
        gotoField.forceActiveFocus()
        gotoField.selectAll()
    }

    height: barOpen ? findBar.implicitHeight : 0
    visible: height > 0

    FindBar {
        id: findBar
        anchors.fill: parent
        visible: root.searchVisible
        valid: root.searchValid
        matchCount: root.matchCount
        onQueryChanged: (text, caseSensitive, wholeWord, regex) =>
                        root.queryChanged(text, caseSensitive, wholeWord, regex)
        onNextRequested: backwards => root.nextRequested(backwards)
        onCloseRequested: root.closeRequested()
    }

    Item {
        anchors.fill: parent
        visible: root.gotoVisible && !root.searchVisible

        PopupSurface {
            anchors.fill: parent
            devicePixelRatio: root.renderDpr
        }

        RowLayout {
            anchors.fill: parent
            anchors.margins: Theme.spaceSm
            spacing: Theme.spaceSm

            UiField {
                id: gotoField
                Layout.fillWidth: true
                placeholderText: qsTr("Go to line")
                validator: IntValidator { bottom: 1; top: Math.max(1, root.lineCount) }
                Keys.onReturnPressed: root.goToLineRequested(parseInt(text))
                Keys.onEscapePressed: root.closeRequested()
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
}
