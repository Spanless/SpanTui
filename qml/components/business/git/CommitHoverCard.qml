pragma Translator: "GitHistoryPane"
pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import SpanTui

// Opens beside the list so it never covers the rows. The owner keeps it open
// while `hovered`, so the pointer can reach the card.
Tip {
    id: card

    property Item row: null
    property var commit: null
    property GitRepository repository: null
    property Item fallbackParent: null
    readonly property bool hovered: cardHover.hovered

    signal showDetailsRequested(var commit)

    readonly property real gap: Theme.spaceSm
    readonly property var info: commit || ({})
    readonly property string hash: info.hash || ""
    readonly property var parentHashes: info.parents || []
    readonly property var refs: info.decorations || []
    property var preview: ({})
    readonly property bool previewReady: preview.hash === hash && hash.length > 0
    readonly property var coAuthors: previewReady ? (preview.coAuthors || []) : []
    readonly property string body: previewReady ? (preview.body || "") : ""
    // Folded body shows a few lines; expanded, the full text scrolls inside
    // a viewport of expandedBodyLines. Each commit starts folded.
    property bool bodyExpanded: false
    readonly property int collapsedBodyLines: 6
    readonly property int expandedBodyLines: 16
    readonly property int unboundedBodyLines: 2147483647
    // Evaluated whenever the anchor row changes; the dock does not move
    // while the pointer sits on it.
    readonly property bool roomLeft: row
        ? row.mapToItem(null, 0, 0).x >= width + gap + margins
        : false

    // Fetch only once the card is actually shown: sweeping the pointer down
    // the list must not spawn a git process per row passed over.
    onHashChanged: {
        bodyExpanded = false
        if (opened)
            loadPreview()
    }
    onOpened: loadPreview()

    function loadPreview() {
        if (!repository || hash.length === 0) {
            preview = ({})
            return
        }
        preview = repository.commitPreview(hash)
        if (preview.hash !== hash)
            repository.requestCommitPreview(hash)
    }

    function relativeAge(value) {
        const stamp = Date.parse(String(value || ""))
        if (isNaN(stamp))
            return ""
        const elapsed = Math.max(0, (Date.now() - stamp) / 1000)
        if (elapsed < 60)
            return qsTr("Just now")
        if (elapsed < 3600)
            return qsTr("%1 min ago").arg(Math.floor(elapsed / 60))
        if (elapsed < 86400)
            return qsTr("%1 h ago").arg(Math.floor(elapsed / 3600))
        if (elapsed < 86400 * 30)
            return qsTr("%1 d ago").arg(Math.floor(elapsed / 86400))
        return ""
    }

    function absoluteDate(value) {
        const date = String(value || "").replace("T", " ")
        return date.length > 16 ? date.substring(0, 16) : date
    }

    Connections {
        target: card.repository
        ignoreUnknownSignals: true
        function onCommitPreviewReady(hash, preview) {
            if (hash === card.hash)
                card.preview = preview
        }
    }

    parent: row || fallbackParent
    x: roomLeft ? -width - gap : (row ? row.width + gap : 0)
    y: 0
    width: Theme.dialogMd
    // A pointer-driven card: it lives exactly as long as the hover does.
    timeout: -1

    background: PopupSurface {
        devicePixelRatio: card.renderDpr
        radius: Theme.radiusMd
    }

    // No padding: the hover probe covers the whole card, on an ancestor of
    // the buttons, which would consume hover before a sibling sees it.
    padding: 0
    contentItem: Item {
        implicitHeight: body.implicitHeight + body.anchors.topMargin
                        + body.anchors.bottomMargin

        HoverHandler { id: cardHover }

        Column {
            id: body
            anchors.fill: parent
            anchors.topMargin: Theme.spaceMd + Theme.spaceSm
            anchors.bottomMargin: Theme.spaceMd
            anchors.leftMargin: Theme.spaceLg
            anchors.rightMargin: Theme.spaceLg
            spacing: Theme.spaceMd

            // Who and when.
            Column {
                width: parent.width
                spacing: Theme.spaceSm

                RowLayout {
                    width: parent.width
                    spacing: Theme.spaceSm
                    UiText {
                        Layout.maximumWidth: parent.width * 0.6
                        text: card.info.author || ""
                        size: "sm"
                        tone: "accent"
                        emphasis: true
                        elide: Text.ElideRight
                    }
                    UiText {
                        Layout.fillWidth: true
                        Layout.minimumWidth: 0
                        readonly property string age: card.relativeAge(card.info.date)
                        text: age.length > 0
                              ? age + "  (" + card.absoluteDate(card.info.date) + ")"
                              : card.absoluteDate(card.info.date)
                        size: "sm"
                        tone: "muted"
                        elide: Text.ElideRight
                    }
                }

                Repeater {
                    model: card.coAuthors
                    delegate: RowLayout {
                        id: coAuthorRow
                        required property var modelData
                        width: parent.width
                        spacing: Theme.spaceSm
                        UiText {
                            Layout.maximumWidth: parent.width * 0.6
                            text: coAuthorRow.modelData.name || coAuthorRow.modelData.email || ""
                            size: "sm"
                            tone: "accent"
                            elide: Text.ElideRight
                        }
                        UiText {
                            Layout.fillWidth: true
                            Layout.minimumWidth: 0
                            text: qsTr("(co-author)")
                            size: "sm"
                            tone: "muted"
                            elide: Text.ElideRight
                        }
                    }
                }
            }

            // What: subject, then the body — folded to a few lines, and
            // once expanded shown in full inside a bounded scroll viewport
            // so a long message never pushes the card off screen.
            Column {
                width: parent.width
                spacing: Theme.spaceSm

                UiText {
                    width: parent.width
                    text: card.info.subject || ""
                    size: "md"
                    emphasis: true
                    wrapMode: Text.Wrap
                }

                DesktopScrollView {
                    id: bodyView
                    readonly property real lineExtent: bodyMetrics.height * bodyText.lineHeight
                    readonly property real fullHeight: bodyText.implicitHeight
                                                       + topPadding + bottomPadding
                    width: parent.width
                    height: card.bodyExpanded
                            ? Math.min(fullHeight, lineExtent * card.expandedBodyLines)
                            : fullHeight
                    visible: card.body.length > 0
                    clip: true
                    padding: 0
                    // Keeps expanded text clear of the overlay bar; it does
                    // not depend on the height, so it cannot feed back.
                    rightPadding: card.bodyExpanded ? Theme.spaceMd : 0
                    contentWidth: availableWidth
                    wheelLineExtent: lineExtent
                    ScrollBar.horizontal.policy: ScrollBar.AlwaysOff

                    UiText {
                        id: bodyText
                        width: bodyView.availableWidth
                        text: card.body
                        size: "sm"
                        wrapMode: Text.Wrap
                        lineHeight: 1.15
                        maximumLineCount: card.bodyExpanded ? card.unboundedBodyLines
                                                            : card.collapsedBodyLines
                        elide: card.bodyExpanded ? Text.ElideNone : Text.ElideRight
                    }

                    FontMetrics {
                        id: bodyMetrics
                        font: bodyText.font
                    }
                }

                ChromeButton {
                    x: -Theme.spaceMd
                    visible: bodyText.truncated || card.bodyExpanded
                    size: "sm"
                    kind: "ghost"
                    text: card.bodyExpanded ? qsTr("Collapse") : qsTr("Expand all")
                    onClicked: card.bodyExpanded = !card.bodyExpanded
                }
            }

            Flow {
                width: parent.width
                visible: card.refs.length > 0
                spacing: Theme.spaceSm
                Repeater {
                    model: card.refs
                    delegate: GitRefBadge {
                        required property var modelData
                        ref: modelData
                        baseColor: Theme.overlaySurface
                    }
                }
            }

            PixelLine {
                width: parent.width
                lineColor: Theme.separator
                opacity: Theme.fadeLg
            }

            // How much changed.
            RowLayout {
                width: parent.width
                spacing: Theme.spaceMd
                UiText {
                    text: !card.previewReady ? qsTr("Loading details…")
                          : card.preview.hasStats
                            ? qsTr("%1 files changed").arg(card.preview.filesChanged)
                            : qsTr("No file changes")
                    size: "sm"
                    tone: "muted"
                }
                DiffStat {
                    visible: card.previewReady && card.preview.hasStats
                    additions: card.preview.insertions || 0
                    deletions: card.preview.deletions || 0
                }
                Item { Layout.fillWidth: true }
            }

            UiText {
                width: parent.width
                visible: card.parentHashes.length > 1
                text: visible
                      ? qsTr("Merge: %1").arg(card.parentHashes
                            .map(function(hash) { return String(hash).substring(0, 7) })
                            .join("  "))
                      : ""
                size: "sm"
                tone: "muted"
                elide: Text.ElideRight
            }

            // Actions.
            RowLayout {
                width: parent.width
                spacing: Theme.spaceSm
                Icon {
                    name: "git-commit"
                    size: Theme.iconSm
                    color: Theme.muted
                }
                MonoText {
                    text: card.info.shortHash || ""
                    px: Theme.uiSm
                    color: Theme.accent
                }
                ChromeButton {
                    size: "sm"
                    kind: "ghost"
                    iconName: "copy"
                    tip: qsTr("Copy commit hash")
                    onClicked: Clipboard.copyText(card.hash)
                }
                Item { Layout.fillWidth: true }
                ChromeButton {
                    size: "sm"
                    kind: "ghost"
                    iconName: "external-link"
                    text: qsTr("Show commit details")
                    onClicked: card.showDetailsRequested(card.commit)
                }
            }
        }
    }
}
