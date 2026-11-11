import QtQuick
import QtQuick.Controls
import SpanTui

// Read-only markdown surface. TextEdit+MarkdownText is expensive to reflow on
// every width change; callers should freeze layout during live resize (see
// PreviewPane.resizeDeferred).
DesktopScrollView {
    id: root
    property string markdown: ""
    property url documentBaseUrl: ""
    // A relative link resolved under the document's directory. The host opens
    // it inside the app; it is never handed to the shell, which would run a
    // linked script or executable.
    signal fileLinkActivated(url target)
    property bool previewActive: visible
    // Remote images wait for a per-document opt-in; the host shows the prompt.
    readonly property int remoteImageCount: markdownRenderer.remoteImageCount
    readonly property bool remoteImagesAllowed: markdownRenderer.remoteImagesAllowed
    function allowRemoteImages() { markdownRenderer.allowRemoteImages() }
    wheelLineExtent: previewMetrics.lineSpacing
    clip: true
    ScrollBar.horizontal.policy: ScrollBar.AlwaysOff
    // The viewport is the single width owner. Binding ScrollView contentWidth
    // back to TextEdit.contentWidth creates a loop because availableWidth also
    // depends on scrollbar geometry.
    contentWidth: availableWidth
    readonly property font documentFont: Appearance.uiFont(Appearance.uiFontSize, Appearance.fontRevision)
    FontMetrics {
        id: previewMetrics
        font: root.documentFont
    }

    onDocumentFontChanged: markdownRenderer.setDocumentFont(documentFont)

    MarkdownDocumentRenderer {
        id: markdownRenderer
        source: root.markdown
        active: root.previewActive
        dark: Theme.dark
        maximumImageWidth: Math.max(1, root.availableWidth - Theme.spaceLg * 2)
        property bool remoteImagesRunning: false

        onRenderingStarted: {
            TaskProgress.start("markdown-render", qsTr("Rendering Markdown"))
            TaskProgress.update("markdown-render", -1, qsTr("Preparing preview"))
        }
        onRenderingFinished: TaskProgress.finish("markdown-render", TaskProgress.Succeeded)
        onRemoteImageCancelled: {
            remoteImagesRunning = false
            TaskProgress.finish("markdown-images", TaskProgress.Cancelled, qsTr("Document changed"))
        }
        onRemoteImageProgressChanged: {
            const pending = pendingRemoteImageCount
            const total = remoteImageTotal
            const failed = remoteImageFailedCount
            const completed = Math.max(0, total - pending)
            if (total <= 0) {
                if (remoteImagesRunning) {
                    remoteImagesRunning = false
                    TaskProgress.finish("markdown-images", TaskProgress.Succeeded)
                }
                return
            }
            if (pending <= 0) {
                remoteImagesRunning = false
                const detail = failed > 0
                             ? qsTr("%1 of %2, %3 failed").arg(completed).arg(total).arg(failed)
                             : qsTr("%1 of %2").arg(completed).arg(total)
                TaskProgress.finish("markdown-images",
                                    failed > 0 ? TaskProgress.Failed
                                               : TaskProgress.Succeeded,
                                    detail)
                return
            }
            if (!remoteImagesRunning) {
                remoteImagesRunning = true
                TaskProgress.start("markdown-images", qsTr("Loading images"))
            }
            TaskProgress.update("markdown-images",
                                total > 0 ? completed / total : -1,
                                failed > 0
                                    ? qsTr("%1 of %2, %3 failed").arg(completed).arg(total).arg(failed)
                                    : qsTr("%1 of %2").arg(completed).arg(total))
        }
    }

    RenderTextEdit {
        id: body
        width: root.availableWidth
        readOnly: true
        selectByMouse: true
        baseUrl: root.documentBaseUrl
        wrapMode: TextEdit.Wrap
        padding: Theme.spaceLg
        color: Theme.text
        // Text selection is the shared textSelection tint (not the neutral
        // row-selection fill).
        selectionColor: Theme.textSelection
        selectedTextColor: Theme.text
        font: root.documentFont
        Component.onCompleted: {
            markdownRenderer.setDocumentFont(root.documentFont)
            markdownRenderer.attach(textDocument)
        }
        onLinkActivated: function(link) {
            if (link.startsWith("#")) {
                const target = markdownRenderer.anchorPosition(link)
                if (target >= 0) {
                    cursorPosition = target
                    ensureCursorVisible()
                    if (root.contentItem) {
                        const targetY = body.cursorRectangle.y
                        const maxScroll = Math.max(0, root.contentItem.contentHeight - root.contentItem.height)
                        root.contentItem.contentY = Math.max(0, Math.min(targetY - Theme.spaceMd, maxScroll))
                    }
                }
                return
            }
            // Match terminal policy: only http(s) leave the process.
            if (link.startsWith("https://") || link.startsWith("http://")) {
                Qt.openUrlExternally(link)
                return
            }
            // Project-relative links resolve to file:// under the document base.
            if (link.indexOf(":") >= 0 || link.startsWith("//"))
                return
            const base = root.documentBaseUrl
            if (!base || base.toString().length === 0)
                return
            const resolved = base.toString().endsWith("/")
                             ? (base.toString() + link)
                             : (base.toString() + "/" + link)
            if (resolved.startsWith("file:"))
                root.fileLinkActivated(resolved)
        }
    }

    Component.onDestruction: {
        TaskProgress.finish("markdown-render", TaskProgress.Cancelled)
        TaskProgress.finish("markdown-images", TaskProgress.Cancelled)
    }
}
