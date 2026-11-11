pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import SpanTui

Item {
    id: root
    required property DocumentManager documentManager
    required property GitRepository gitRepository
    signal documentActivated()
    signal fileLinkActivated(url target)
    // When the outer preview column is being dragged, freeze markdown
    // layout width so TextEdit Markdown reflow does not run every frame.
    property bool resizeDeferred: false
    property bool backgroundEnabled: true

    property real frozenLayoutWidth: 0
    property real frozenLayoutHeight: 0
    readonly property real renderDpr: PixelGrid.safeDpr(Screen.devicePixelRatio)
    readonly property real hairline: PixelGrid.hairline(renderDpr)
    readonly property real snappedTabbarHeight: PixelGrid.snapSize(Theme.tabbarH, renderDpr)
    readonly property real snappedTabInset: PixelGrid.snapSize(Theme.spaceMd, renderDpr)

    readonly property bool layoutFrozen: resizeDeferred
    readonly property real layoutWidth: layoutFrozen && frozenLayoutWidth > 0
                                        ? frozenLayoutWidth : documentBody.width
    readonly property real layoutHeight: layoutFrozen && frozenLayoutHeight > 0
                                         ? frozenLayoutHeight : documentBody.height
    readonly property bool hasDocument: documentManager.activeIndex >= 0

    function freezeLayout() {
        if (documentBody.width > 0)
            frozenLayoutWidth = documentBody.width
        if (documentBody.height > 0)
            frozenLayoutHeight = documentBody.height
    }

    function thawLayout() {
        if (!resizeDeferred) {
            frozenLayoutWidth = 0
            frozenLayoutHeight = 0
        }
    }

    onResizeDeferredChanged: {
        if (resizeDeferred)
            freezeLayout()
        else
            thawLayout()
    }
    // Document plane: the window paper, one tier below panel chrome. A plain
    // square fill, so a Rectangle rather than the SDF surface (see
    // TerminalSurface).
    Rectangle {
        anchors.fill: parent
        visible: root.backgroundEnabled
        color: Theme.surfaceBase
        z: Theme.zBelow
    }

    ColumnLayout {
        anchors.fill: parent
        spacing: 0

        // Each viewer tile owns one ordered document tab strip.
        Item {
            id: previewTabStrip
            Layout.fillWidth: true
            Layout.preferredHeight: root.snappedTabbarHeight

            readonly property real tabWidth: PixelGrid.snapSize(Theme.sizeLg * 5,
                                                                root.renderDpr)

            function ensureVisible(index: int): void {
                if (!root.documentManager || index < 0 || !documentTabs || index >= documentTabs.count
                        || !tabFlick || tabFlick.width <= 0)
                    return
                const tab = documentTabs.itemAt(index)
                if (!tab)
                    return
                const left = root.snappedTabInset + tab.x
                const right = left + tab.width
                if (left < tabFlick.contentX)
                    tabFlick.contentX = Math.max(0, left)
                else if (right > tabFlick.contentX + tabFlick.width)
                    tabFlick.contentX = Math.max(0, right - tabFlick.width)
            }

            Timer {
                id: tabScrollTimer
                interval: 0
                repeat: false
                onTriggered: {
                    if (previewTabStrip && root.documentManager)
                        previewTabStrip.ensureVisible(root.documentManager.activeIndex)
                }
            }

            // One shared Tab/content boundary. Capsules sit in the parent
            // header strip and never cover this seam.
            PixelLine {
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.bottom: parent.bottom
                devicePixelRatio: root.renderDpr
                lineColor: Theme.border
            }

            DesktopFlickable {
                id: tabFlick
                anchors.fill: parent
                contentWidth: tabRow.width + root.snappedTabInset * 2
                contentHeight: height
                boundsBehavior: Flickable.StopAtBounds
                interactive: contentWidth > width
                clip: true

                Row {
                    id: tabRow
                    x: root.snappedTabInset
                    height: tabFlick.height
                    spacing: 0

                    Repeater {
                        id: documentTabs
                        model: root.documentManager
                        delegate: WorkspaceTab {
                            id: documentTab
                            required property int index
                            required property string path
                            required property string name
                            required property bool missing
                            width: previewTabStrip.tabWidth
                            height: tabRow.height
                            text: name
                            useFileIcon: true
                            fileName: name
                            textColor: missing ? Theme.warning : Theme.text
                            statusVisible: missing
                            statusColor: Theme.warning
                            active: documentTab.index === root.documentManager.activeIndex
                            onActivated: {
                                root.documentManager.activate(documentTab.index)
                                root.documentActivated()
                                if (editorSurface.visible) {
                                    editorSurface.focusPreview()
                                }
                            }
                            onCloseRequested: root.documentManager.closeDocument(documentTab.index)
                        }
                    }


                }
            }

            Connections {
                target: root.documentManager
                function onActiveDocumentChanged(): void {
                    if (previewTabStrip && previewTabStrip.visible) {
                        tabScrollTimer.restart()
                    }
                }
            }
        }

        InlineBanner {
            Layout.fillWidth: true
            active: root.hasDocument && root.documentManager.activeMissing
            text: qsTr("The file is missing on disk.")
        }

        // Fetching a remote image reveals the user's IP to its host, so each
        // document asks first; switching or reopening asks again.
        InlineBanner {
            Layout.fillWidth: true
            active: root.hasDocument && markdownPreview.visible
                    && markdownPreview.remoteImageCount > 0
                    && !markdownPreview.remoteImagesAllowed
            tone: "info"
            text: qsTr("This document has %n remote image(s). Loading them reveals your IP address to external servers.",
                       "", markdownPreview.remoteImageCount)
            actionText: qsTr("Load")
            onActionTriggered: markdownPreview.allowRemoteImages()
        }

        // Editor / preview surface — flush to the pane, no nested card.
        Item {
            id: documentBody
            Layout.fillWidth: true
            Layout.fillHeight: true
            clip: true
            visible: root.hasDocument

            Item {
                id: layoutHost
                width: root.layoutWidth
                height: root.layoutHeight
                x: 0
                y: 0

                CodePreview {
                    id: editorSurface
                    documentIndex: root.documentManager.activeIndex
                    manager: root.documentManager
                    repository: root.gitRepository
                    documentActive: root.hasDocument && visible
                    anchors.fill: parent
                    visible: root.hasDocument && !root.documentManager.activeMarkdown
                }

                MarkdownPreview {
                    id: markdownPreview
                    anchors.fill: parent
                    visible: root.documentManager.activeMarkdown
                    previewActive: visible
                    markdown: root.documentManager.activeText
                    documentBaseUrl: root.documentManager.activeBaseUrl
                    onFileLinkActivated: function(target) { root.fileLinkActivated(target) }
                }
            }

        }

        // Empty state fills the body when nothing is open.
        EmptyState {
            Layout.fillWidth: true
            Layout.fillHeight: true
            active: !root.hasDocument
            icon: "files"
            title: qsTr("Nothing to preview")
            subtitle: qsTr("Select a file in the file tree")
        }


    }

    Connections {
        target: root.documentManager
        function onDocumentActivated(): void {
            root.documentActivated()
            if (editorSurface.visible) {
                editorSurface.focusPreview()
            }
        }
    }
}
