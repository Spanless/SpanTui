import QtQuick
import SpanTui

// Floating palette card for one scheme color. Opens anchored to the swatch
// that requested it, edits live (colorEdited fires on every change so the
// editor draft and preview track the drag), and commits on close. The old
// swatch resets to the color the popup opened with.
AnchorPanel {
    id: root

    property string title: ""
    property color value: "#ffffff"
    property color originalColor: "#ffffff"
    property bool alphaEnabled: false
    // Assigning a hex string here coerces it to a color before decomposition.
    property color coerced: "#ffffff"
    readonly property real planeWidth: 216
    readonly property real planeHeight: 132
    readonly property real barHeight: 12

    signal colorEdited(color color)

    function openFor(item, colorTitle, color, withAlpha) {
        root.title = colorTitle
        root.alphaEnabled = withAlpha === true
        root.originalColor = color
        root.setValue(color)
        openBelow(item)
    }

    function setValue(c) {
        root.coerced = c
        if (root.coerced.hsvHue >= 0)
            hueSlider.position = root.coerced.hsvHue
        planeMarker.s = root.coerced.hsvSaturation
        planeMarker.v = root.coerced.hsvValue
        alphaSlider.position = root.coerced.a
        hexField.syncFromColor(root.currentColor())
    }

    function currentColor() {
        return Qt.hsva(hueSlider.position, planeMarker.s, planeMarker.v,
                       alphaSlider.position)
    }

    function emitEdited() {
        root.value = root.currentColor()
        if (!hexField.activeFocus)
            hexField.syncFromColor(root.value)
        root.colorEdited(root.value)
    }

    padding: Theme.spaceLg
    anchorAlign: "center"
    anchorFlip: true
    anchorGap: Theme.spaceMd

    Column {
        id: contentRoot
        spacing: Theme.spaceMd

        UiText {
            visible: root.title.length > 0
            text: root.title
            size: "md"
            elide: Text.ElideRight
        }

        // Saturation/value plane: a hue base washed by white (left to right)
        // and black (top to bottom) composites the classic HSV square.
        Item {
            width: root.planeWidth
            height: root.planeHeight
            clip: true

            Rectangle {
                anchors.fill: parent
                color: Qt.hsva(hueSlider.position, 1, 1, 1)
            }
            Rectangle {
                anchors.fill: parent
                gradient: Gradient {
                    orientation: Gradient.Horizontal
                    GradientStop { position: 0; color: "#ffffffff" }
                    GradientStop { position: 1; color: "#00ffffff" }
                }
            }
            Rectangle {
                anchors.fill: parent
                gradient: Gradient {
                    orientation: Gradient.Vertical
                    GradientStop { position: 0; color: "#00000000" }
                    GradientStop { position: 1; color: "#ff000000" }
                }
            }

            Rectangle {
                id: planeMarker
                property real s: 0
                property real v: 1
                x: s * (parent.width - width)
                y: (1 - v) * (parent.height - height)
                width: 12          // style-ok: plane marker ring
                height: 12         // style-ok: plane marker ring
                radius: width / 2
                color: "#00000000"
                border.width: 2
                border.color: s < 0.5 && v > 0.6 ? "#cc000000" : "#e6ffffff"
            }

            MouseArea {
                anchors.fill: parent
                function apply(mouse) {
                    planeMarker.s = Math.min(1, Math.max(0, mouse.x / width))
                    planeMarker.v = Math.min(1, Math.max(0, 1 - mouse.y / height))
                    root.emitEdited()
                }
                onPressed: mouse => apply(mouse)
                onPositionChanged: mouse => {
                    if (pressed)
                        apply(mouse)
                }
            }
        }

        // Hue strip.
        Item {
            width: root.planeWidth
            height: root.barHeight
            clip: true

            Rectangle {
                anchors.fill: parent
                radius: root.barHeight / 2
                gradient: Gradient {
                    orientation: Gradient.Horizontal
                    GradientStop { position: 0.000; color: "#ff0000" }
                    GradientStop { position: 0.167; color: "#ffff00" }
                    GradientStop { position: 0.333; color: "#00ff00" }
                    GradientStop { position: 0.500; color: "#00ffff" }
                    GradientStop { position: 0.667; color: "#00ff00" }
                    GradientStop { position: 0.833; color: "#ff00ff" }
                    GradientStop { position: 1.000; color: "#ff0000" }
                }
            }

            Rectangle {
                width: 4           // style-ok: slider handle
                height: parent.height + 4
                radius: Theme.radiusXs
                y: -2
                x: hueSlider.position * (parent.width - width) - 1
                color: "#00000000"
                border.color: "#e6ffffff"
                border.width: 2
            }

            MouseArea {
                id: hueSlider
                property real position: 0
                anchors.fill: parent
                cursorShape: Qt.PointingHandCursor
                function apply(mouse) {
                    position = Math.min(1, Math.max(0, mouse.x / width))
                    root.emitEdited()
                }
                onPressed: mouse => apply(mouse)
                onPositionChanged: mouse => {
                    if (pressed)
                        apply(mouse)
                }
            }
        }

        // Opacity strip; the mid-gray base stands in for a checkerboard so
        // transparency reads without a pattern dependency.
        Item {
            visible: root.alphaEnabled
            width: root.planeWidth
            height: root.barHeight
            clip: true

            Rectangle {
                anchors.fill: parent
                radius: root.barHeight / 2
                color: "#8a8a8a"
            }
            Rectangle {
                anchors.fill: parent
                radius: root.barHeight / 2
                gradient: Gradient {
                    orientation: Gradient.Horizontal
                    GradientStop { position: 0; color: "#00000000" }
                    GradientStop {
                        position: 1
                        color: Qt.hsva(hueSlider.position, planeMarker.s,
                                      planeMarker.v, 1)
                    }
                }
            }

            Rectangle {
                width: 4           // style-ok: slider handle
                height: parent.height + 4
                radius: Theme.radiusXs
                y: -2
                x: alphaSlider.position * (parent.width - width) - 1
                color: "#00000000"
                border.color: "#e6ffffff"
                border.width: 2
            }

            MouseArea {
                id: alphaSlider
                property real position: 1
                anchors.fill: parent
                cursorShape: Qt.PointingHandCursor
                function apply(mouse) {
                    position = Math.min(1, Math.max(0, mouse.x / width))
                    root.emitEdited()
                }
                onPressed: mouse => apply(mouse)
                onPositionChanged: mouse => {
                    if (pressed)
                        apply(mouse)
                }
            }
        }

        Row {
            spacing: Theme.spaceMd

            CrispSurface {
                // Original color; clicking it reverts the live edit.
                width: 36          // style-ok: color swatch
                height: 24         // style-ok: color swatch
                radius: Theme.radiusXs
                devicePixelRatio: root.renderDpr
                fillTopColor: root.originalColor
                fillMiddleColor: root.originalColor
                fillBottomColor: root.originalColor
                strokeEnabled: true
                strokeWidthPx: 1
                strokeColor: Theme.border

                MouseArea {
                    anchors.fill: parent
                    cursorShape: Qt.PointingHandCursor
                    onClicked: {
                        root.setValue(root.originalColor)
                        root.emitEdited()
                    }
                }
            }

            CrispSurface {
                width: 36          // style-ok: color swatch
                height: 24         // style-ok: color swatch
                radius: Theme.radiusXs
                devicePixelRatio: root.renderDpr
                fillTopColor: root.value
                fillMiddleColor: root.value
                fillBottomColor: root.value
                strokeEnabled: true
                strokeWidthPx: 1
                strokeColor: Theme.border
            }

            UiField {
                id: hexField
                role: "mono"
                size: "sm"
                width: root.planeWidth - 36 * 2 - Theme.spaceMd * 2
                horizontalAlignment: TextInput.AlignHCenter
                validator: RegularExpressionValidator {
                    regularExpression: /^#(?:[0-9a-fA-F]{6}|[0-9a-fA-F]{8})$/
                }
                property bool syncing: false
                function syncFromColor(color) {
                    syncing = true
                    const full = color.toString() // #aarrggbb
                    text = color.a >= 1 ? full.substring(full.length - 7) : full
                    syncing = false
                }
                onTextEdited: {
                    if (syncing || !acceptableInput)
                        return
                    root.setValue(text)
                    root.emitEdited()
                }
            }
        }
    }
}
