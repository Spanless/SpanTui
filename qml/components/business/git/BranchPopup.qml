import QtQuick
import QtQuick.Controls
import SpanTui

AnchorPanel {
    id: root
    required property GitRepository repository
    readonly property int listWidth: 320

    width: listWidth
    height: Math.min(440, parent ? parent.height - Theme.spaceLg : 440)
    padding: 0
    clip: true
    autoSizeToContent: false

    onOpening: {
        refsTree.clearFilterAndFocus()
        Qt.callLater(function() { refsTree.clearFilterAndFocus() })
    }

    background: Item {}

    contentItem: Item {
        PopupSurface {
            anchors.fill: parent
            devicePixelRatio: root.renderDpr
        }

        GitRefsTree {
            id: refsTree
            repository: root.repository
            anchors.fill: parent
            onReferenceActivated: root.close()
        }
    }
}
