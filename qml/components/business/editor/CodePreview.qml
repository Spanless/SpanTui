import QtQuick
import QtQuick.Controls
import SpanTui

Item {
    id: root

    // Search bar floats above the editor surface inside this preview scope.
    readonly property int searchBarZ: 40

    required property DocumentManager manager
    required property GitRepository repository
    property int documentIndex: manager ? manager.activeIndex : -1
    property bool documentActive: manager && documentIndex === manager.activeIndex
    property bool searchVisible: false
    property bool gotoVisible: false
    property bool baseRequestPending: true
    property var highlightLines: ({})
    property int cursorOffset: -1

    readonly property string filePath: manager ? manager.pathAt(documentIndex) : ""
    readonly property string fileName: manager && documentIndex >= 0
                                       ? manager.activeName : ""
    readonly property font codeFont: Appearance.editorFont(Appearance.editorFontSize, Appearance.fontRevision)

    function focusPreview() {
        linesView.forceActiveFocus()
    }

    function closeSearchUi() {
        searchVisible = false
        gotoVisible = false
        focusPreview()
    }

    function activeSearch() {
        searchVisible = true
        gotoVisible = false
        searchBar.focusSearch()
    }

    function openGoto() {
        gotoVisible = true
        searchVisible = false
        searchBar.focusGoto()
    }

    function requestBase() {
        if (baseRequestPending && repository && repository.available
                && filePath.length > 0) {
            baseRequestPending = false
            repository.requestBaseContent(filePath)
        }
    }

    function rebuildRows() {
        if (!manager || documentIndex < 0) {
            editorController.clearDocument()
            return
        }
        editorController.clearBaseContent()
        const value = manager.textAt(documentIndex)
        editorController.setText(value)
        rebuildHighlights()
        requestBase()
        Qt.callLater(function() {
            const initialCursor = manager ? manager.cursorAt(documentIndex) : 0
            if (initialCursor > 0) {
                const line = editorController.lineForOffset(initialCursor)
                const lastNl = value.slice(0, initialCursor).lastIndexOf("\n")
                const col = lastNl >= 0 ? (initialCursor - lastNl) : (initialCursor + 1)
                linesView.positionAtLine(line)
                editorController.setCursorInfo(line, col, 0)
            } else {
                linesView.positionAtLine(1)
                editorController.setCursorInfo(1, 1, 0)
            }
        })
    }

    function rebuildHighlights() {
        highlightLines = editorController.highlightLines()
    }

    function nextSearch(backwards) {
        const match = editorController.nextMatch(root.cursorOffset, backwards)
        if (!match || match.start === undefined)
            return
        root.cursorOffset = match.start
        const line = editorController.lineForOffset(match.start)
        linesView.positionAtLine(line)
        editorController.setCursorInfo(line, 1, Number(match.end) - Number(match.start))
        rebuildHighlights()
    }

    onFilePathChanged: {
        baseRequestPending = true
        Qt.callLater(rebuildRows)
    }
    onDocumentIndexChanged: {
        baseRequestPending = true
        Qt.callLater(rebuildRows)
    }
    Component.onCompleted: Qt.callLater(rebuildRows)

    Connections {
        target: root.manager
        function onActiveDocumentChanged() {
            // Keep documentIndex declaratively bound to manager.activeIndex;
            // a command assignment here would silently detach it.
            root.baseRequestPending = true
            Qt.callLater(root.rebuildRows)
        }
    }
    Connections {
        target: repository
        function onBaseContentReady(path, text, existsInHead) {
            if (path === root.filePath)
                editorController.setBaseContent(text, existsInHead)
        }
        function onStatusMapChanged() {
            root.baseRequestPending = true
            Qt.callLater(root.requestBase)
        }
        function onBusyChanged() {
            if (root.baseRequestPending)
                Qt.callLater(root.requestBase)
        }
    }

    EditorController { id: editorController }
    Connections {
        target: editorController
        function onSearchChanged() { root.rebuildHighlights() }
    }

    TextLinesView {
        id: linesView
        objectName: "workspaceCodePreview"
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: parent.top
        anchors.bottom: statusBar.top
        mode: "file"
        model: editorController.lineModel
        highlightLines: root.highlightLines
        emptyText: qsTr("Empty file")
        maxImplicitHeight: height
        focus: true
        onCurrentLineChanged: {
            editorController.setCursorInfo(linesView.currentLine, 1, 0)
            if (manager && documentIndex >= 0) {
                manager.setDocumentCursor(documentIndex,
                    editorController.goToLine(linesView.currentLine))
            }
        }
        Keys.onPressed: event => {
            // A chord may carry several actions; each bound one runs.
            let handled = false
            if (Shortcuts.matches("editor.copyLine", event.key, event.modifiers)) {
                if (linesView.currentLine <= editorController.lineModel.count)
                    Clipboard.copyText(editorController.lineModel.lineText(linesView.currentLine))
                handled = true
            }
            if (Shortcuts.matches("editor.goToLine", event.key, event.modifiers)) {
                root.openGoto(); handled = true
            }
            if (Shortcuts.matches("editor.find", event.key, event.modifiers)) {
                root.activeSearch(); handled = true
            }
            if (handled) {
                event.accepted = true; return
            }
            if (event.key === Qt.Key_Up) {
                if (linesView.currentLine > 1) {
                    linesView.setCurrentLine(linesView.currentLine - 1, false)
                }
                event.accepted = true; return
            }
            if (event.key === Qt.Key_Down) {
                if (linesView.currentLine < editorController.lineModel.count) {
                    linesView.setCurrentLine(linesView.currentLine + 1, false)
                }
                event.accepted = true; return
            }
        }
    }

    PaneLoading {
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: parent.top
        anchors.bottom: statusBar.top
        active: root.manager && root.manager.loadingAt(root.documentIndex)
        message: qsTr("Loading file…")
    }

    EditorSearchBar {
        id: searchBar
        anchors.top: parent.top
        anchors.topMargin: Theme.spaceMd
        anchors.right: parent.right
        anchors.rightMargin: Theme.spaceMd
        width: Math.max(0, Math.min(620, root.width - Theme.spaceMd * 2,
                                   Math.max(360, root.width * 0.46)))
        searchVisible: root.searchVisible
        gotoVisible: root.gotoVisible
        searchValid: editorController.searchValid
        matchCount: editorController.matchCount
        lineCount: editorController.lineCount
        cursorLine: editorController.cursorLine
        onQueryChanged: (text, caseSensitive, wholeWord, regex) => {
            root.cursorOffset = -1
            editorController.setSearch(text, caseSensitive, wholeWord, regex)
        }
        onNextRequested: backwards => root.nextSearch(backwards)
        onGoToLineRequested: line => {
            linesView.positionAtLine(line)
            editorController.setCursorInfo(line, 1, 0)
            root.gotoVisible = false
            root.focusPreview()
        }
        onCloseRequested: root.closeSearchUi()
        z: root.searchBarZ
    }

    EditorStatusBar {
        id: statusBar
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        fileName: root.fileName
        cursorLine: editorController.cursorLine
        cursorColumn: editorController.cursorColumn
        selectionLength: editorController.selectionLength
    }
}
