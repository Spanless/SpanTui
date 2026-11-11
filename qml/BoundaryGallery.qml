import QtQuick
import SpanTui

// Deterministic real-window fixture for the D3D11/RHI pixel test. The primary
// surface uses deliberately high-contrast colors; the second card exercises
// the production PaneCard composition for visual QA without affecting probes.
Window {
    id: root
    width: 640
    height: 520
    visible: false
    flags: Qt.Window | Qt.FramelessWindowHint
    color: darkMode ? "#101218" : "#eceff4"

    property bool darkMode: true
    onDarkModeChanged: Theme.dark = darkMode
    Component.onCompleted: Theme.dark = darkMode

    CrispSurface {
        objectName: "pixelProbeSurface"
        x: 96
        y: 72
        width: 448
        height: 276
        radius: 24
        fillEnabled: true
        strokeEnabled: true
        strokeWidthPx: 1
        fillTopColor: "#25313d"
        fillMiddleColor: "#25313d"
        fillBottomColor: "#25313d"
        strokeColor: "#f4f7ff"
    }

    // Premultiplied QColor probe. A second RGB*alpha in the shader turns this
    // half-white swatch visibly dark gray.
    CrispSurface {
        x: 16
        y: 360
        width: 120
        height: 44
        radius: 0
        fillTopColor: "#80ffffff"
        fillMiddleColor: "#80ffffff"
        fillBottomColor: "#80ffffff"
        strokeEnabled: false
    }

    // Elevation recipes for visual DPI QA (no probes read this strip): a
    // sunken track holding a concentric raised thumb, a focus ring, and a
    // floating surface.
    CrispSurface {
        x: 508
        y: 380
        width: 116
        height: 28
        radius: Theme.radiusMd
        elevation: "sunken"

        CrispSurface {
            x: Theme.thumbInset
            y: Theme.thumbInset
            width: 56
            height: parent.height - Theme.thumbInset * 2
            radius: Theme.radiusSm
            elevation: "raised"
        }
    }
    CrispSurface {
        x: 508
        y: 424
        width: 116
        height: 28
        radius: Theme.radiusSm
        elevation: "raised"
        focusRing: true
    }
    CrispSurface {
        x: 508
        y: 464
        width: 116
        height: 28
        radius: Theme.radiusLg
        elevation: "floating"
    }
}
