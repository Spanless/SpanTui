pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import SpanTui

Item {
    id: root

    property color color: Theme.terminalBackdrop
    property bool interactionAnimationsEnabled: true
    property int contentLifecycle: 0
    property bool backgroundEnabled: true
    property string sessionId: ""
    property TerminalSession namedSession: null
    readonly property TerminalSession boundSession: root.sessionId.length > 0
                                                    ? root.namedSession
                                                    : TerminalManager.activeSession

    function refreshNamedSession(): void {
        const next = root.sessionId.length > 0
                     ? TerminalManager.session(root.sessionId)
                     : null
        if (root.namedSession !== next)
            root.namedSession = next
    }

    onSessionIdChanged: refreshNamedSession()


    Connections {
        target: TerminalManager
        enabled: root.sessionId.length > 0
        function onSessionStatesChanged(): void { root.refreshNamedSession() }
    }

    readonly property real renderDpr: PixelGrid.safeDpr(Screen.devicePixelRatio)
    // Same sizing as the file viewer's find bar: a compact overlay that
    // covers the right part of the grid, not the whole row.
    readonly property int findBarMaxWidth: 620
    readonly property int findBarMinWidth: 360

    readonly property bool hasActiveSession: root.boundSession !== null
    readonly property bool unreadOutput: terminalView.unreadOutput
    readonly property bool terminalRunning: hasActiveSession && root.boundSession.running

    onTerminalRunningChanged: {
        if (terminalRunning)
            Qt.callLater(focusTerminal)
    }

    function scrollToBottom(): void {
        terminalView.scrollToBottom()
    }

    function focusTerminal(): void {
        terminalView.forceActiveFocus()
    }

    property bool findOpen: false

    // Seeds the query with the first selected line, as editors do.
    function openFind(): void {
        const selected = terminalView.selectedText().split("\n")[0]
        root.findOpen = true
        findBar.focusSearch(selected)
        if (findBar.text.length > 0)
            findBar.emitQuery()
    }

    function closeFind(): void {
        root.findOpen = false
        terminalView.clearSearch()
        root.focusTerminal()
    }

    // Visual plane under the grid: the opaque scheme backdrop. Default cells
    // emit no background geometry and reveal this plane; explicit ANSI
    // backgrounds and reverse video still become opaque cell runs.
    // A plain non-antialiased Rectangle: edges land on the same physical
    // pixels as CrispSurface's snap, and an opaque fill takes the renderer's
    // opaque pass instead of running the SDF shader over the whole grid.
    Rectangle {
        anchors.fill: parent
        visible: root.backgroundEnabled
        color: root.color
        z: Theme.zBelow
    }

    // Always reserved, so the column count does not change when history
    // appears.
    Item {
        id: scrollGutter
        anchors.top: parent.top
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        width: Theme.spaceMd + Theme.spaceSm
        visible: root.hasActiveSession

        PixelLine {
            anchors.left: parent.left
            anchors.top: parent.top
            anchors.bottom: parent.bottom
            vertical: true
            devicePixelRatio: root.renderDpr
        }

        SlimScrollBar {
            id: historyScrollBar
            anchors.fill: parent
            anchors.topMargin: Theme.spaceMd
            anchors.bottomMargin: Theme.spaceMd
            orientation: Qt.Vertical
            centered: true
            // Its own lane covers nothing, so the bar stays shown whenever
            // there is history to scroll.
            policy: terminalView.scrollbackLines > 0 ? ScrollBar.AlwaysOn : ScrollBar.AlwaysOff
            // Scrollback is not a Flickable: reveal on the user's own
            // scrolling, not on output that merely grows the history.
            onTerminalScrollOffsetChanged: pulse()
            readonly property real viewportRows: terminalView.cellHeight > 0
                                                  ? terminalView.height / terminalView.cellHeight : 1
            readonly property real totalRows: Math.max(1, terminalView.scrollbackLines + viewportRows)
            readonly property real terminalScrollbackLines: terminalView.scrollbackLines
            readonly property real terminalScrollOffset: terminalView.scrollOffset
            size: Math.min(1, viewportRows / totalRows)

            Binding {
                target: historyScrollBar
                property: "position"
                value: Math.max(0, Math.min(historyScrollBar.terminalScrollbackLines / historyScrollBar.totalRows,
                                  (historyScrollBar.terminalScrollbackLines - historyScrollBar.terminalScrollOffset) / historyScrollBar.totalRows))
                when: !historyScrollBar.pressed
            }

            // Fractional offsets keep a dragged bar moving the text smoothly.
            onPositionChanged: if (pressed) {
                const requested = terminalView.scrollbackLines - position * totalRows
                terminalView.setScrollOffset(Math.max(
                    0, Math.min(terminalView.scrollbackLines, requested)))
            }
        }
    }

    Item {
        id: body
        anchors.left: parent.left
        anchors.right: scrollGutter.left
        anchors.top: parent.top
        anchors.bottom: parent.bottom
        anchors.leftMargin: Theme.spaceMd
        anchors.bottomMargin: Theme.spaceMd
        anchors.topMargin: Theme.spaceMd
        clip: true

        TerminalView {
            id: terminalView
            anchors.fill: parent
            anchors.rightMargin: Theme.spaceMd
            visible: root.visible && root.hasActiveSession
            session: root.boundSession
            font: Appearance.terminalFont
            foregroundColor: Theme.terminalForeground
            // This is a semantic reference, not a painted full-pane fill.
            // Explicit ANSI backgrounds and reverse video still become opaque
            // cell runs; ordinary default-background cells reveal the plane.
            backgroundColor: Theme.terminalBackdrop
            cursorColor: Theme.terminalCursor
            cursorStyle: TerminalSettings.cursorStyle
            cursorAnimationEnabled: !Appearance.reduceMotion
            smoothScrolling: !Appearance.reduceMotion
            maximumFrameRate: TerminalSettings.refreshRate
            selectionColor: Theme.terminalSelection
            shortcutBindings: Shortcuts.terminalBindings
            searchMatchColor: Theme.terminalSearchMatch
            searchCurrentColor: Theme.terminalSearchCurrent

            onShortcutActivated: id => {
                if (id === "terminal.find")
                    root.openFind()
            }
            ansiPalette: Theme.terminalAnsi
            presentationState: root.contentLifecycle

            onSessionChanged: {
                if (session !== null && (root.activeFocus || activeFocus))
                    forceActiveFocus()
            }

            onLinkActivated: function(link) {
                if (link.startsWith("https://") || link.startsWith("http://"))
                    Qt.openUrlExternally(link)
            }

            onContextMenuRequested: function(x, y, link) {
                const allowed = link.startsWith("https://") || link.startsWith("http://")
                terminalMenu.openAt(terminalView, x, y, [
                    { text: qsTr("Copy"), icon: "copy", enabled: terminalView.hasSelection,
                      action: function() { terminalView.copySelection() } },
                    { text: qsTr("Paste"), icon: "paste",
                      action: function() { terminalView.pasteClipboard() } },
                    { text: qsTr("Find…"), icon: "search",
                      action: function() { root.openFind() } },
                    { separator: true },
                    { text: qsTr("Open link"), icon: "external-link", enabled: link.length > 0 && allowed,
                      action: function() { Qt.openUrlExternally(link) } },
                    { text: qsTr("Copy link"), icon: "copy", enabled: link.length > 0,
                      action: function() { terminalView.copyText(link) } },
                    { separator: true },
                    { text: qsTr("Clear output"), icon: "eraser",
                      action: function() {
                          if (root.boundSession)
                              root.boundSession.clearDisplay()
                      } }
                ])
            }
        }

        FindBar {
            id: findBar
            anchors.top: parent.top
            anchors.right: parent.right
            anchors.rightMargin: Theme.spaceMd
            width: Math.max(0, Math.min(root.findBarMaxWidth, parent.width - Theme.spaceMd * 2,
                                        Math.max(root.findBarMinWidth, parent.width * 0.46)))
            height: implicitHeight
            visible: root.findOpen && root.hasActiveSession
            z: Theme.zRaised
            // libghostty finds literal text only, ASCII letters ignoring case.
            matchOptionsVisible: false
            matchCount: terminalView.searchMatchCount
            currentIndex: terminalView.searchCurrentIndex
            truncated: terminalView.searchPending
            onQueryChanged: text => terminalView.setSearch(text)
            onNextRequested: backwards => terminalView.findNext(backwards)
            onCloseRequested: root.closeFind()
        }

        EmptyState {
            anchors.fill: parent
            active: !root.hasActiveSession
            icon: "terminal"
            title: qsTr("No terminal open")
            subtitle: qsTr("Use the new-terminal button to start a session")
        }

        EmptyState {
            anchors.fill: parent
            active: root.hasActiveSession && root.boundSession
                    && root.boundSession.launchAttempted && !root.terminalRunning
                    && !root.boundSession.starting
            icon: root.boundSession && root.boundSession.failed ? "alert" : "terminal"
            iconColor: root.boundSession && root.boundSession.failed ? Theme.danger : Theme.muted
            title: root.boundSession && root.boundSession.startupError.length > 0
                   ? root.boundSession.startupError
                   : root.boundSession && root.boundSession.failed
                     ? qsTr("Terminal exited with code %1").arg(root.boundSession.exitCode)
                     : qsTr("Terminal exited")
            actionIcon: "play"
            actionText: root.boundSession && root.boundSession.startupError.length > 0
                        ? qsTr("Reconnect")
                        : root.boundSession && root.boundSession.hasCommandLine
                          ? qsTr("Re-run command")
                          : qsTr("Restart terminal")
            onActionTriggered: {
                const session = root.boundSession
                if (!session)
                    return
                if (session.hasCommandLine && session.startupError.length === 0) {
                    rerunConfirm.openFor(
                        qsTr("Re-run command"),
                        qsTr("Start the session command again?"),
                        qsTr("Re-run"),
                        function() { TerminalManager.restartSession(session.id) })
                    return
                }
                TerminalManager.restartSession(session.id)
            }
        }
    }

    AppContextMenu { id: terminalMenu }
    ConfirmDialog {
        id: rerunConfirm
        danger: false
    }
    Component.onCompleted: {
        refreshNamedSession()
        terminalView.commitViewport()
        terminalView.forceActiveFocus()
    }

}
