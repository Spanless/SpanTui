import QtQuick
import QtQuick.Controls
import SpanTui

ApplicationWindow {
    id: window

    width: 960  // style-ok: window default geometry
    height: 620  // style-ok: window default geometry
    minimumWidth: 480  // style-ok: window default geometry
    minimumHeight: 300  // style-ok: window default geometry
    visible: true
    title: (Qt.application.displayName
            ? (Qt.application.displayName + " Terminal")
            : qsTr("SpanTui Terminal"))
    color: Theme.surfaceBase
    font: Appearance.uiFont(Appearance.uiFontSize, Appearance.fontRevision)

    function syncAppearanceTheme() {
        Theme.dark = Appearance.effectiveDark
        Theme.uiFontSize = Appearance.uiFontSize
        Theme.uiAuxFontSize = Appearance.uiAuxFontSize
        Theme.editorFontSize = Appearance.editorFontSize
    }

    function syncTerminalTheme() {
        Theme.applyScheme(TerminalSettings.surfaceColors)
    }

    function syncAnimationPolicy() {
        Theme.animateColors = !Appearance.reduceMotion
                              && !(WindowTransition && WindowTransition.active)
    }

    function syncTheme() {
        syncAppearanceTheme()
        syncTerminalTheme()
        syncAnimationPolicy()
    }

    Connections {
        target: Appearance
        function onAppearanceChanged() { window.syncAppearanceTheme() }
        function onReduceMotionChanged() { window.syncAnimationPolicy() }
    }

    Connections {
        target: TerminalSettings
        function onAppearanceChanged() { window.syncTerminalTheme() }
    }

    Connections {
        target: WindowTransition
        function onPhaseChanged() { window.syncAnimationPolicy() }
    }

    TerminalSurface {
        id: surface
        anchors.fill: parent
        color: Theme.terminalBackdrop
        backgroundEnabled: true
        // Independent-window transitions use the same presentation lifecycle
        // as workspace containers: active geometry is moving, stable is normal.
        contentLifecycle: WindowTransition && WindowTransition.active ? 1 : 0
    }

    Component.onCompleted: {
        window.syncTheme()
        surface.focusTerminal()
    }
}
