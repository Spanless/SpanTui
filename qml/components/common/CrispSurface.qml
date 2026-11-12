pragma ComponentBehavior: Bound

import QtQuick
import SpanTui

// One RHI quad for analytic rounded fills, rims, and shadows. All metric
// properties ending in Px are physical pixels; radii remain logical pixels so
// the visual proportions track the rest of the QML layout.
// `elevation` pre-fills fill/rim/shadow from the Theme elevation recipes
// (design-system.md §5); "none" leaves every channel to the caller, and a
// call site may still override a single channel of a preset. `focusRing`
// takes over the rim and shadow channels for keyboard/edit focus: accent
// rim plus a zero-offset accent falloff, replacing the elevation shadow.
Item {
    id: root

    property real devicePixelRatio: PixelGrid.safeDpr(Screen.devicePixelRatio)
    property real radius: 0
    property real topLeftRadius: radius
    property real topRightRadius: radius
    property real bottomRightRadius: radius
    property real bottomLeftRadius: radius

    // "none" | "sunken" | "raised" | "floating"
    property string elevation: "none"
    readonly property bool _sunken: elevation === "sunken"
    readonly property bool _raised: elevation === "raised"
    readonly property bool _floating: elevation === "floating"
    property bool focusRing: false

    property bool fillEnabled: true
    property bool strokeEnabled: focusRing || _raised || _floating
    property bool shadowEnabled: focusRing || _raised || _floating
    property real shapeInsetPx: 0
    property real strokeWidthPx: 1
    property real strokeStrength: 1

    property color fillTopColor: _sunken ? Theme.surfaceInset
                                 : _raised ? Theme.raisedFillTop
                                 : _floating ? Theme.overlaySurface
                                 : "transparent"
    property color fillMiddleColor: _raised ? Theme.raisedFill : fillTopColor
    property color fillBottomColor: fillMiddleColor
    property color strokeColor: focusRing ? Theme.accent
                                : _raised ? Theme.raisedRim
                                : _floating ? Theme.floatingRim
                                : "transparent"
    property color shadowColor: focusRing ? Theme.accent
                                : _raised ? Theme.shadowRaised
                                : _floating ? Theme.shadowFloating
                                : "transparent"
    // Gaussian sigma in physical pixels; visible falloff is roughly 3x this
    // value, so callers must not treat it as a CSS box-shadow blur radius.
    property real shadowBlurPx: focusRing ? Theme.focusRingBlurPx
                                : _raised ? Theme.raisedShadowBlur * devicePixelRatio
                                : _floating ? Theme.floatingShadowBlur * devicePixelRatio
                                : 4
    property real shadowOffsetYPx: focusRing ? 0
                                   : _raised ? Theme.raisedShadowOffsetY * devicePixelRatio
                                   : _floating ? Theme.floatingShadowOffsetY * devicePixelRatio
                                   : 2
    property real shadowStrength: focusRing ? Theme.focusRingStrength : 1

    readonly property real hairline: PixelGrid.hairline(devicePixelRatio)
    readonly property real shadowPaddingPx: shadowEnabled
                                                 ? Math.ceil(shadowBlurPx * 3 + Math.abs(shadowOffsetYPx))
                                                 : 0
    // The quad reaches one physical pixel past the shape (and its shadow).
    // The shader snaps each edge to the nearest pixel boundary; a far edge
    // on an exact half pixel (an odd logical coordinate at 150%) rounds
    // outward, and that last pixel row or column lay outside an unpadded
    // quad, whose bottom/right edge rasterization excludes it — the bottom
    // and right rim came out thin or missing.
    readonly property real quadPaddingPx: shadowPaddingPx + 1
    readonly property bool softwareRenderer: GraphicsInfo.api === GraphicsInfo.Software
    readonly property bool fallbackActive: softwareRenderer
                                                   || gpuSurface.status === ShaderEffect.Error
    readonly property string shaderLog: gpuSurface.log

    readonly property bool gpuActive: !root.fallbackActive
                                      && (root.fillEnabled || root.strokeEnabled
                                          || root.shadowEnabled)

    ShaderEffect {
        id: gpuSurface
        anchors.fill: parent
        anchors.margins: -root.quadPaddingPx / root.devicePixelRatio
        visible: root.gpuActive
        blending: true

        property vector2d quadSizePx: Qt.vector2d(width * root.devicePixelRatio,
                                                  height * root.devicePixelRatio)
        property vector2d shapeSizePx: Qt.vector2d(root.width * root.devicePixelRatio,
                                                   root.height * root.devicePixelRatio)
        property vector4d cornerRadiiPx: Qt.vector4d(
            root.topLeftRadius * root.devicePixelRatio,
            root.topRightRadius * root.devicePixelRatio,
            root.bottomRightRadius * root.devicePixelRatio,
            root.bottomLeftRadius * root.devicePixelRatio)
        property real shapeInsetPx: root.shapeInsetPx
        property real strokeWidthPx: root.strokeWidthPx
        property real fillEnabledValue: root.fillEnabled ? 1 : 0
        property real strokeEnabledValue: root.strokeEnabled ? 1 : 0
        property real strokeStrengthValue: root.strokeStrength
        property real shadowEnabledValue: root.shadowEnabled ? 1 : 0
        property real shadowBlurPx: root.shadowBlurPx
        property real shadowOffsetYPx: root.shadowOffsetYPx
        property real shadowStrengthValue: root.shadowStrength
        property color fillTopColor: root.fillTopColor
        property color fillMiddleColor: root.fillMiddleColor
        property color fillBottomColor: root.fillBottomColor
        property color strokeColor: root.strokeColor
        property color shadowColor: root.shadowColor

        vertexShader: "qrc:/shaders/crisp_surface.vert.qsb"
        fragmentShader: "qrc:/shaders/crisp_surface.frag.qsb"
    }

    // Capability fallback, not a second outline. It is instantiated only when
    // the software scene graph is active or the baked shader failed to load.
    Loader {
        anchors.fill: parent
        active: root.fallbackActive && (root.fillEnabled || root.strokeEnabled)
        sourceComponent: Rectangle {
            color: root.fillEnabled ? root.fillMiddleColor : "transparent"
            topLeftRadius: root.topLeftRadius
            topRightRadius: root.topRightRadius
            bottomRightRadius: root.bottomRightRadius
            bottomLeftRadius: root.bottomLeftRadius
            border.width: root.strokeEnabled ? root.hairline : 0
            border.color: Qt.rgba(root.strokeColor.r,
                                  root.strokeColor.g,
                                  root.strokeColor.b,
                                  root.strokeColor.a * Math.max(0, Math.min(1, root.strokeStrength)))
            antialiasing: true
            gradient: root.fillEnabled ? fallbackGradient : null

            Gradient {
                id: fallbackGradient
                GradientStop { position: 0.0; color: root.fillTopColor }
                GradientStop { position: 0.54; color: root.fillMiddleColor }
                GradientStop { position: 1.0; color: root.fillBottomColor }
            }
        }
    }
}
