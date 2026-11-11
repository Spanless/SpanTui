import QtQuick
import QtQuick.Layouts
import SpanTui

// File/folder identity glyph for the explorer tree, Git rows and document Tabs.
// FileIconTheme resolves a catalog stem; color follows the adjacent label.
Item {
    id: root

    property string fileName: ""
    property bool isDir: false
    property bool expanded: false
    property bool isRoot: false
    property real size: Theme.iconMd
    property color color: Theme.text

    implicitWidth: size
    implicitHeight: size
    width: size
    height: size
    onWidthChanged: if (width !== size) width = size
    onHeightChanged: if (height !== size) height = size
    onSizeChanged: { width = size; height = size }

    Layout.fillWidth: false
    Layout.fillHeight: false
    Layout.preferredWidth: size
    Layout.preferredHeight: size
    Layout.maximumWidth: size
    Layout.maximumHeight: size
    Layout.minimumWidth: size
    Layout.minimumHeight: size

    Icon {
        anchors.centerIn: parent
        size: root.size
        name: FileIconTheme.iconName(root.fileName, root.isDir, root.expanded, root.isRoot)
        color: root.color
    }
}
