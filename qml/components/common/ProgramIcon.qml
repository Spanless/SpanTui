import QtQuick
import SpanTui

// The icon embedded in a program or .ico file, read through the
// "programicon" image provider (ProgramIconProvider). Full color, never
// tinted: it is the program's own mark, like the shells in Windows Terminal.
// An empty `path`, or a file without an icon, leaves the box blank.
Image {
    id: root

    property string path: ""
    property real size: Theme.iconMd
    readonly property real renderDpr: PixelGrid.safeDpr(Screen.devicePixelRatio)

    width: size
    height: size
    source: path.length > 0
            ? "image://programicon/" + encodeURIComponent(path.replace(/\\/g, "/"))
            : ""
    sourceSize: Qt.size(Math.ceil(size * renderDpr), Math.ceil(size * renderDpr))
    fillMode: Image.PreserveAspectFit
    smooth: true
    asynchronous: true
}
