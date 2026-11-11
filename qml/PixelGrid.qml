pragma Singleton

import QtQuick

// Physical-pixel geometry helpers. DPR is deliberately supplied by the caller:
// a process-global value would be wrong as soon as a window crosses monitors.
QtObject {
    function safeDpr(value) {
        return Math.max(1, Number.isFinite(value) ? value : 1)
    }

    function hairline(dpr) {
        return 1 / safeDpr(dpr)
    }

    function snap(value, dpr) {
        const scale = safeDpr(dpr)
        return Math.round(value * scale) / scale
    }

    function snapSize(value, dpr) {
        return Math.max(0, snap(value, dpr))
    }
}
