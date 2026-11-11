pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Layouts
import QtQuick.Shapes
import SpanTui

// See docs/ui/icons.md. No layer.smooth/mipmap: they blur the glyph. Edges
// soften only on an origin between physical pixels, which PixelSnap removes.
// The per-part view exists only while moving or active.
Item {
    id: root

    property string name: ""
    property real size: Theme.iconMd
    property color color: Theme.text
    // Design-space stroke (24×24). Optical ≈ 1.0–1.25 CSS-px at size 16–20
    // — the light line weight of platform symbol sets.
    property real strokeWidth: 1.5
    property bool hovered: false
    property bool pressed: false
    property bool active: false
    // Off on a fill of the motion's tint colour (accent/danger pill), or the
    // tinted parts vanish.
    property bool tinted: true
    // Every host exposes UiIcons (application/runtime/qml_types.h).
    readonly property bool glyphFilled: name.length > 0 && UiIcons.filled(name)
    readonly property bool animated: name.length > 0 && UiIcons.animated(name)
    readonly property bool moving: motionLoader.item !== null && motionLoader.item.engaged
    // Stand-in while a part list and its frame swap during a name change.
    readonly property var restFrame: ({ matrix: Qt.matrix4x4(), opacity: 0, draw: 1,
                                        tint: 0, fill: 0, path: "" })

    function tintColor(role: string): color {
        switch (role) {
        case "accent": return Theme.accent
        case "danger": return Theme.danger
        case "success": return Theme.success
        case "warning": return Theme.warning
        case "cursor": return Theme.terminalCursor
        }
        return root.color
    }

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

    Item {
        id: glyph
        width: 24   // style-ok: vector source grid
        height: 24  // style-ok: vector source grid
        // Scale to display size, then shift the origin onto a physical pixel
        // so edges stay one sharp pixel at fractional scale factors
        // (docs/ui/pixel-boundaries.md).
        transform: [
            Scale {
                xScale: root.size / 24
                yScale: root.size / 24
            },
            PixelSnap { item: glyph }
        ]

        Shape {
            anchors.fill: parent
            visible: !root.moving
            antialiasing: true
            // CurveRenderer: high-quality AA without an intermediate layer texture.
            preferredRendererType: Shape.CurveRenderer
            vendorExtensionsEnabled: false

            ShapePath {
                strokeColor: root.glyphFilled ? "transparent" : root.color
                strokeWidth: root.glyphFilled ? 0 : root.strokeWidth
                fillColor: root.glyphFilled ? root.color : "transparent"
                capStyle: ShapePath.RoundCap
                joinStyle: ShapePath.RoundJoin
                PathSvg {
                    path: root.name.length > 0 ? UiIcons.path(root.name) : ""
                }
            }
        }

        Loader {
            id: motionLoader
            anchors.fill: parent
            active: root.animated
            sourceComponent: motionView
        }
    }

    // A Component (not an inline component) so the view keeps this file's ids.
    Component {
        id: motionView

        Item {
            readonly property alias engaged: motion.engaged

            IconMotion {
                id: motion
                catalog: UiIcons
                name: root.name
                hovered: root.hovered
                active: root.active
                reduceMotion: Appearance.reduceMotion
            }

            Connections {
                target: root
                function onPressedChanged(): void {
                    if (root.pressed)
                        motion.press()
                }
            }

            Repeater {
                model: motion.engaged ? motion.parts : []

                Shape {
                    id: part
                    required property var modelData
                    required property int index
                    readonly property var f: index < motion.frame.length ? motion.frame[index]
                                                                          : root.restFrame
                    readonly property bool drawn: !modelData.solid && f.draw < 0.999
                                                  && modelData.length > 0
                    readonly property color ink: root.tinted && modelData.tint.length > 0
                                                 && f.tint > 0
                                                 ? Theme.mix(root.color,
                                                             root.tintColor(modelData.tint), f.tint)
                                                 : root.color

                    width: 24   // style-ok: vector source grid
                    height: 24  // style-ok: vector source grid
                    visible: modelData.path.length > 0 && f.opacity > 0
                             && (modelData.solid ? f.fill > 0 : f.draw > 0.002)
                    opacity: f.opacity
                    antialiasing: true
                    preferredRendererType: Shape.CurveRenderer
                    vendorExtensionsEnabled: false
                    transform: Matrix4x4 { matrix: part.f.matrix }

                    ShapePath {
                        strokeColor: part.modelData.solid ? "transparent" : part.ink
                        strokeWidth: part.modelData.solid ? 0 : root.strokeWidth
                        fillColor: part.modelData.solid
                                   ? Theme.withAlpha(part.ink, part.ink.a * part.f.fill)
                                   : "transparent"
                        fillRule: ShapePath.OddEvenFill
                        capStyle: ShapePath.RoundCap
                        joinStyle: ShapePath.RoundJoin
                        // Draw trims the stroke from its start: one dash as
                        // long as the visible fraction, then a gap past the end.
                        strokeStyle: part.drawn ? ShapePath.DashLine : ShapePath.SolidLine
                        dashPattern: part.drawn
                                     ? [part.modelData.length * part.f.draw / root.strokeWidth,
                                        (part.modelData.length + 1) / root.strokeWidth]
                                     : [1, 0]
                        PathSvg {
                            path: part.modelData.morph ? part.f.path : part.modelData.path
                        }
                    }
                }
            }
        }
    }
}
