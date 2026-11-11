pragma Singleton

import QtQuick

QtObject {
    id: theme

    function composite(top, bottom) {
        top = slotColor(top)
        bottom = slotColor(bottom)
        const alpha = top.a + bottom.a * (1.0 - top.a)
        if (alpha <= 0.0)
            return Qt.rgba(0, 0, 0, 0)
        return Qt.rgba((top.r * top.a + bottom.r * bottom.a * (1.0 - top.a)) / alpha,
                       (top.g * top.a + bottom.g * bottom.a * (1.0 - top.a)) / alpha,
                       (top.b * top.a + bottom.b * bottom.a * (1.0 - top.a)) / alpha,
                       alpha)
    }

    function mix(a, b, t) {
        a = slotColor(a)
        b = slotColor(b)
        return Qt.rgba(a.r + (b.r - a.r) * t,
                       a.g + (b.g - a.g) * t,
                       a.b + (b.b - a.b) * t,
                       a.a + (b.a - a.a) * t)
    }

    // Palettes may hold hex strings. Never coerce through a mutable property:
    // a write+read inside a binding makes every caller depend on it and loop.
    function slotColor(c) {
        if (typeof c === "string" && c.length >= 7 && c[0] === "#")
            return Qt.rgba(parseInt(c.substr(1, 2), 16) / 255,
                           parseInt(c.substr(3, 2), 16) / 255,
                           parseInt(c.substr(5, 2), 16) / 255,
                           c.length >= 9 ? parseInt(c.substr(7, 2), 16) / 255
                                         : 1.0)
        if (c !== undefined && c !== null && typeof c.r === "number")
            return c
        return Qt.rgba(0, 0, 0, 0)
    }

    function hexOf(c) {
        const h = v => ("0" + Math.round(Math.max(0, Math.min(1, v)) * 255)
                        .toString(16)).slice(-2)
        return "#" + h(c.r) + h(c.g) + h(c.b)
    }

    function withAlpha(c, alpha) {
        c = slotColor(c)
        return Qt.rgba(c.r, c.g, c.b, alpha)
    }

    function _lin(c) {
        return c <= 0.04045 ? c / 12.92 : Math.pow((c + 0.055) / 1.055, 2.4)
    }

    function luminance(c) {
        return 0.2126 * _lin(c.r) + 0.7152 * _lin(c.g) + 0.0722 * _lin(c.b)
    }

    function contrastRatio(a, b) {
        const la = luminance(a)
        const lb = luminance(b)
        return (Math.max(la, lb) + 0.05) / (Math.min(la, lb) + 0.05)
    }

    // Keep in step with ensureMinimumContrast() in terminal_color.cpp.
    function readable(fg, bg, minimumRatio) {
        fg = slotColor(fg)
        if (!bg || fg.a <= 0 || minimumRatio <= 1.0)
            return fg
        if (contrastRatio(fg, bg) >= minimumRatio)
            return fg
        const black = Qt.rgba(0, 0, 0, 1)
        const white = Qt.rgba(1, 1, 1, 1)
        const target = contrastRatio(black, bg) >= contrastRatio(white, bg)
                       ? black : white
        if (contrastRatio(target, bg) < minimumRatio)
            return target
        let low = 0.0, high = 1.0, best = target
        for (let step = 0; step < 14; ++step) {
            const mid = (low + high) * 0.5
            const candidate = mix(fg, target, mid)
            if (contrastRatio(candidate, bg) >= minimumRatio) {
                best = candidate
                high = mid
            } else {
                low = mid
            }
        }
        return best
    }

    property bool dark: true
    // Off until Main applies the persisted theme, so a cold start does not
    // animate every surface.
    property bool animateColors: false

    readonly property int durFast: 140
    readonly property int durMid: 260
    readonly property int durTheme: 320
    readonly property int durSlow: 420
    readonly property int colorAnimMs: animateColors ? durTheme : 0
    readonly property int durStagger: 28
    readonly property var easeEmphasized: [0.23, 1, 0.32, 1, 1, 1]
    readonly property real pressScale: 0.97
    readonly property int tipDelay: 550
    readonly property int tipDelayQuick: 250
    readonly property int tipTimeout: 6000

    readonly property real fadeSm: 0.30
    readonly property real fadeMd: 0.55
    readonly property real fadeLg: 0.85
    readonly property real opacityDisabled: 0.4
    // Physical-pixel sigma.
    readonly property real focusRingBlurPx: 1.5
    readonly property real focusRingStrength: fadeMd

    readonly property int zBelow: -1
    readonly property int zRaised: 2
    readonly property int zWorkspace: 1
    readonly property int zChrome: 100
    readonly property int zToast: 1000
    readonly property int zModal: 1100

    readonly property int radiusXs: 4
    readonly property int radiusSm: 6
    readonly property int radiusMd: 8
    readonly property int radiusLg: 10
    readonly property int radiusXl: 14
    readonly property int thumbInset: radiusMd - radiusSm

    readonly property int spaceSm: 4
    readonly property int spaceMd: 8
    readonly property int spaceLg: 16
    readonly property int sizeSm: Math.max(20, uiFontSize + 8)
    readonly property int sizeMd: Math.max(28, uiFontSize + 16)
    readonly property int sizeLg: Math.max(36, uiFontSize + 24)
    function fitText(rung, lineHeight) {
        return Math.max(rung, Math.ceil(lineHeight) + spaceSm)
    }
    readonly property int titlebarH: sizeMd + spaceSm * 2
    // Side pane headers and the session-tab strip share one height so their
    // bottom dividers align.
    readonly property int paneHeaderH: sizeSm + spaceMd * 2
    readonly property int tabbarH: paneHeaderH
    readonly property int handleW: 10
    // Physical pixels; the resting seam is always one device pixel.
    readonly property int handleHoverPx: 3
    readonly property int scrollPad: 2
    readonly property int scrollEdgeGap: 1

    readonly property int dialogSm: 320
    readonly property int dialogMd: 420
    readonly property int dialogLg: 640

    readonly property int fieldMd: 250
    readonly property int formLabelW: 200

    readonly property int emptyContentW: 240

    readonly property int iconSm: 16
    readonly property int iconMd: 18
    readonly property int iconLg: 24

    // Shorter than the chevron column so nested rows stay near their parent.
    readonly property int treeDepthStep: 14

    // Initial values match AppearanceSettings::laneSpec defaults.
    property int uiFontSize: 14
    property int uiAuxFontSize: 12
    property int formHintFontSize: 10
    property int editorFontSize: 13

    readonly property int uiSm: uiAuxFontSize
    readonly property int uiMd: uiFontSize
    readonly property int uiHint: formHintFontSize

    // Seeds mirror the Span built-ins and defaultAnsiPalette() in
    // terminal_color.cpp so the first frame is already coherent.
    property color paper: dark ? "#131211" : "#f5f3ef"
    property color plate: dark ? "#1b1a18" : "#fbfaf8"
    property color ink: dark ? "#ebe7df" : "#26231e"
    property color accent: dark ? "#93a5d8" : "#4661b3"
    property color stateHover: dark ? "#1aebe7df" : "#0f26231e"
    property color stateSelected: dark ? "#3d93a5d8" : "#244661b3"
    property color statePressed: dark ? "#29ebe7df" : "#1c26231e"

    property color terminalForeground: dark ? "#ebe7df" : "#26231e"
    property color terminalBackdrop: dark ? "#131211" : "#f5f3ef"
    property color terminalCursor: "#d86575"
    property color terminalSelection: dark ? "#3d93a5d8" : "#2e4661b3"
    property var terminalAnsi: dark ? [
        "#2b2926", "#f06b7a", "#5ed4a0", "#f4c36a",
        "#75a7ff", "#c596ff", "#58cbd4", "#e4e0d9",
        "#76716a", "#ff8d99", "#78e2b4", "#ffd37d",
        "#95bbff", "#d0b1ff", "#7de1e8", "#ffffff"
    ] : [
        "#34312c", "#b4233c", "#147a52", "#8a5a00",
        "#285eab", "#6d3fa3", "#0f6f7a", "#625d55",
        "#56514a", "#d03a4c", "#168a5b", "#8a5a00",
        "#1e5fc0", "#7b45ba", "#0c7d89", "#211e1a"
    ]

    readonly property color surfaceBase: paper
    readonly property color surfaceRaised: plate
    // No rim, so the tone alone must separate it from plate.
    readonly property color surfaceInset: mix(plate, ink, 0.09)
    // surfaceInset is too faint to hold a white knob.
    readonly property color switchTrackOff: mix(surfaceInset, ink, dark ? 0.10 : 0.06)
    readonly property color overlaySurface: dark ? mix(plate, ink, 0.18)
                                                 : mix(plate, "#ffffff", 0.5)

    // Must stay aligned with defaultSurfaceForeground() in terminal_color.cpp.
    property color text: dark ? "#ebe7df" : "#26231e"
    property color muted: dark ? "#a29d94" : "#716b62"

    readonly property color border: withAlpha(ink, dark ? 0.22 : 0.17)
    readonly property color separator: withAlpha(ink, dark ? 0.24 : 0.18)

    function stateLayer(layer, tone) {
        if (!tone || tone === "neutral")
            return layer
        const hue = tone === "danger" ? danger
                  : tone === "warning" ? warning
                  : tone === "success" ? success
                  : accent
        return withAlpha(hue, slotColor(layer).a)
    }
    function stateFill(surface, state, tone) {
        const selected = state === "selected" || state === "selectedHover"
                         || state === "selectedPressed"
        let fill = selected ? composite(stateLayer(stateSelected, tone), surface)
                            : slotColor(surface)
        if (state === "hover" || state === "selectedHover")
            fill = composite(stateLayer(stateHover, tone), fill)
        else if (state === "pressed" || state === "selectedPressed")
            fill = composite(stateLayer(statePressed, tone), fill)
        return fill
    }
    readonly property color hover: stateFill(plate, "hover")
    readonly property color hoverOnOverlay: stateFill(overlaySurface, "hover")
    readonly property color pressed: stateFill(plate, "pressed")
    readonly property color pressedOnInset: stateFill(surfaceInset, "pressed")
    readonly property color accentSoft: mix(plate, accent, dark ? 0.26 : 0.20)
    readonly property color textSelection: accentSoft
    readonly property color accentText: (0.2126 * accent.r + 0.7152 * accent.g + 0.0722 * accent.b) > 0.55
                                        ? "#10131a" : "#ffffff"
    // A solid fill is the state itself, so it shifts instead of taking a
    // scheme state layer.
    function solidFill(fill, state) {
        if (state === "hover")
            return mix(fill, "#ffffff", 0.10)
        if (state === "pressed")
            return mix(fill, "#000000", 0.14)
        return slotColor(fill)
    }
    readonly property color accentHover: solidFill(accent, "hover")
    readonly property color accentPressed: solidFill(accent, "pressed")
    readonly property color accentTop: mix(accent, "#ffffff", dark ? 0.08 : 0.12)
    readonly property color selection: stateFill(plate, "selected")
    readonly property color selectionHover: stateFill(plate, "selectedHover")
    readonly property color selectionPressed: stateFill(plate, "selectedPressed")

    property color danger: dark ? "#f0808e" : "#b52d43"
    readonly property color dangerSoft: mix(plate, danger, dark ? 0.26 : 0.20)
    readonly property color dangerPressed: solidFill(danger, "pressed")
    readonly property color warningSoft: mix(plate, warning, dark ? 0.26 : 0.20)
    readonly property color terminalSearchMatch: withAlpha(warning, dark ? 0.30 : 0.22)
    readonly property color terminalSearchCurrent: withAlpha(warning, dark ? 0.62 : 0.48)
    property color dangerText: "#fffafb"
    property color success: dark ? "#69c798" : "#247650"
    property color warning: dark ? "#dca45e" : "#8a581f"
    // Badge symbols are cut-outs, so these need no text contrast; brighter
    // than the text-tuned status colors above and not scheme-derived.
    readonly property color statusSuccess: "#67c23a"
    readonly property color statusError: "#f56c6c"
    readonly property color statusWarning: "#e6a23c"
    readonly property color statusInfo: dark ? "#a6a9ad" : "#909399"
    property color gitAdded: dark ? "#70c79a" : "#1f7a50"
    property color gitModified: dark ? "#78a9e6" : "#2867a8"
    property color gitDeleted: dark ? "#f0838f" : "#b3263e"
    property color gitRenamed: dark ? "#b79ae8" : "#7050a0"
    readonly property color gitIgnored: muted
    property var gitGraph: dark ? [
        "#78a9e6", "#70c79a", "#f0838f", "#b79ae8",
        "#58cbd4", "#e892c0", "#4cc3a8", "#93a5c4"
    ] : [
        "#2867a8", "#1f7a50", "#b3263e", "#7050a0",
        "#0f6f7a", "#8a2f6e", "#177a64", "#5a6b8c"
    ]

    readonly property color editorGutterText: mix(ink, surfaceBase, 0.52)
    readonly property color editorCurrentLine: mix(surfaceBase, ink, dark ? 0.08 : 0.05)
    readonly property color editorSearchMatch: mix(surfaceBase, accent, dark ? 0.34 : 0.26)
    function diffTint(underlay, kind) {
        const hue = kind === "delete" ? gitDeleted
                  : (kind === "add" || kind === "change") ? gitAdded
                  : null
        return hue ? mix(underlay, hue, dark ? 0.18 : 0.12) : slotColor(underlay)
    }

    readonly property color syntaxKeyword: Qt.tint(
        text, withAlpha(accent, 0.72))
    readonly property color syntaxType: Qt.tint(
        text, withAlpha(gitRenamed, 0.68))
    readonly property color syntaxString: Qt.tint(
        text, withAlpha(success, 0.70))
    readonly property color syntaxNumber: Qt.tint(
        text, withAlpha(gitDeleted, 0.68))
    readonly property color syntaxComment: muted
    readonly property color syntaxFunction: Qt.tint(
        text, withAlpha(gitModified, 0.68))
    readonly property color syntaxProperty: Qt.tint(
        text, withAlpha(slotColor(gitGraph[4]), 0.66))

    readonly property color categoryBuild: dark ? "#24344a" : "#e2ebf6"
    readonly property color categoryTest: dark ? "#243d34" : "#e2f1e9"
    readonly property color categoryDependency: dark ? "#353139" : "#eee9f1"
    readonly property color categoryDocs: dark ? "#3a3428" : "#f3eee1"
    readonly property color categoryAssets: dark ? "#293a3d" : "#e3f0f1"
    readonly property color categoryConfig: dark ? "#343434" : "#ecece8"

    readonly property color raisedFill: dark ? mix(plate, ink, 0.20)
                                             : mix(plate, "#ffffff", 0.85)
    readonly property color raisedFillTop: dark ? mix(raisedFill, ink, 0.06) : raisedFill
    readonly property color raisedFillHover: stateFill(raisedFill, "hover")
    readonly property color raisedFillPressed: stateFill(raisedFill, "pressed")
    // A rim strong enough to read as a line turns the thumb back into an
    // outlined box; lift comes from the shadow.
    readonly property color raisedRim: withAlpha(ink, dark ? 0.08 : 0.035)
    // Buttons have no track behind them, so on a near-white plate the edge
    // alone must separate them from the page.
    readonly property color buttonRim: withAlpha(ink, dark ? 0.16 : 0.10)
    readonly property color shadowRaised: dark ? "#59000000" : "#26000000"
    readonly property real raisedShadowBlur: 1
    readonly property real raisedShadowOffsetY: 1
    readonly property color floatingRim: withAlpha(ink, dark ? 0.24 : 0.18)
    readonly property color shadowFloating: dark ? "#c00a0812" : "#463b4659"
    readonly property real floatingShadowBlur: 6
    readonly property real floatingShadowOffsetY: 2
    readonly property color scrim: dark ? "#a0000000" : "#281f1a2a"

    function applyScheme(colors) {
        terminalForeground = colors.foreground
        terminalBackdrop = colors.background
        terminalCursor = colors.cursor
        terminalSelection = colors.selection
        terminalAnsi = colors.ansi
        if (colors.uiBackground)
            paper = colors.uiBackground
        if (colors.uiPlate)
            plate = colors.uiPlate
        if (colors.uiAccent)
            accent = colors.uiAccent
        if (colors.uiHover)
            stateHover = colors.uiHover
        if (colors.uiSelection)
            stateSelected = colors.uiSelection
        if (colors.uiPressed)
            statePressed = colors.uiPressed
        ink = colors.foreground

        // Derive from the target values: reading back an animated palette
        // property mid-flight bakes the previous scheme into these roles.
        const targetInk = slotColor(colors.foreground)
        const targetRaised = colors.uiPlate ? slotColor(colors.uiPlate)
                                            : slotColor(plate)
        const targetAccent = colors.uiAccent ? slotColor(colors.uiAccent)
                                             : slotColor(accent)
        const ansi = terminalAnsi
        const pick = (index, fallback) =>
            (ansi && ansi.length === 16 && ansi[index])
            ? slotColor(ansi[index]) : slotColor(fallback)
        const lo = dark ? 0 : 8   // dark schemes use bright slots 9–14
        const guard = c => readable(c, targetRaised, 2.5)

        const newText = readable(targetInk, targetRaised, 4.5)
        // Secondary copy is body text: AA 4.5, not the 3.0 large-text floor.
        const newMuted = readable(pick(8, mix(targetInk, targetRaised, 0.5)),
                                  targetRaised, 4.5)
        const newDanger = guard(pick(9 - lo, danger))
        const newSuccess = guard(pick(10 - lo, success))
        const newWarning = guard(pick(11 - lo, warning))
        const newGitModified = guard(pick(12 - lo, gitModified))
        const newGitRenamed = guard(pick(13 - lo, gitRenamed))
        const newAccentLane = guard(pick(14 - lo, targetAccent))
        const newTextLane = guard(pick(15 - lo, newText))

        text = newText
        muted = newMuted
        danger = newDanger
        success = newSuccess
        warning = newWarning
        gitAdded = newSuccess
        gitModified = newGitModified
        gitDeleted = newDanger
        gitRenamed = newGitRenamed
        // CSS strings: the commit graph feeds them to Canvas.
        gitGraph = [
            hexOf(newGitModified), hexOf(newSuccess), hexOf(newDanger),
            hexOf(newGitRenamed), hexOf(newAccentLane),
            hexOf(newWarning), hexOf(newTextLane), hexOf(newMuted)
        ]
        dangerText = contrastRatio("#10131a", newDanger)
                   >= contrastRatio("#ffffff", newDanger) ? "#10131a" : "#ffffff"
    }

    Behavior on paper { ColorAnimation { duration: theme.colorAnimMs } }
    Behavior on plate { ColorAnimation { duration: theme.colorAnimMs } }
    Behavior on ink { ColorAnimation { duration: theme.colorAnimMs } }
    Behavior on accent { ColorAnimation { duration: theme.colorAnimMs } }
    Behavior on stateHover { ColorAnimation { duration: theme.colorAnimMs } }
    Behavior on stateSelected { ColorAnimation { duration: theme.colorAnimMs } }
    Behavior on statePressed { ColorAnimation { duration: theme.colorAnimMs } }
    Behavior on terminalBackdrop { ColorAnimation { duration: theme.colorAnimMs } }
    Behavior on terminalForeground { ColorAnimation { duration: theme.colorAnimMs } }
    Behavior on terminalCursor { ColorAnimation { duration: theme.colorAnimMs } }
    Behavior on terminalSelection { ColorAnimation { duration: theme.colorAnimMs } }
    Behavior on text { ColorAnimation { duration: theme.colorAnimMs } }
    Behavior on muted { ColorAnimation { duration: theme.colorAnimMs } }
    Behavior on danger { ColorAnimation { duration: theme.colorAnimMs } }
    Behavior on dangerText { ColorAnimation { duration: theme.colorAnimMs } }
    Behavior on success { ColorAnimation { duration: theme.colorAnimMs } }
    Behavior on warning { ColorAnimation { duration: theme.colorAnimMs } }
    Behavior on gitAdded { ColorAnimation { duration: theme.colorAnimMs } }
    Behavior on gitModified { ColorAnimation { duration: theme.colorAnimMs } }
    Behavior on gitDeleted { ColorAnimation { duration: theme.colorAnimMs } }
    Behavior on gitRenamed { ColorAnimation { duration: theme.colorAnimMs } }
}
