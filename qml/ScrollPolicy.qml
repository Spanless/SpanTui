pragma Singleton

import QtQuick

// For every Flickable. A mouse drag-flick would fight text selection and the
// scrollbar, so only touch flicks. Nested viewports scroll the one under the
// pointer; a gesture it cannot use goes to the next ancestor.
QtObject {
    readonly property int acceptedButtons: Qt.NoButton
    readonly property int boundsBehavior: Flickable.StopAtBounds
    readonly property int boundsMovement: Flickable.StopAtBounds
    function finite(value, fallback) {
        return typeof value === "number" && isFinite(value) ? value : fallback
    }

    function axisBounds(viewport, horizontal) {
        const origin = finite(horizontal ? viewport.originX : viewport.originY, 0)
        const leadingMargin = finite(horizontal ? viewport.leftMargin : viewport.topMargin, 0)
        const trailingMargin = finite(horizontal ? viewport.rightMargin : viewport.bottomMargin, 0)
        const contentExtent = Math.max(0, finite(horizontal ? viewport.contentWidth : viewport.contentHeight, 0))
        const viewportExtent = Math.max(0, finite(horizontal ? viewport.width : viewport.height, 0))
        const minimum = origin - leadingMargin
        const maximum = Math.max(minimum, origin + contentExtent + trailingMargin - viewportExtent)
        return { minimum: minimum, maximum: maximum, extent: viewportExtent }
    }

    function angleDistance(angleDelta, lineExtent, wheelScrollLines, viewportExtent) {
        if (angleDelta === 0 || wheelScrollLines === 0)
            return 0
        const perNotch = wheelScrollLines < 0 || wheelScrollLines > 10000
                       ? viewportExtent * 0.9
                       : Math.max(1, lineExtent) * wheelScrollLines
        return -angleDelta / 120 * perNotch
    }

    function moveAxis(viewport, horizontal, distance, bounds) {
        if (distance === 0 || bounds.maximum <= bounds.minimum)
            return false
        const property = horizontal ? "contentX" : "contentY"
        const current = finite(viewport[property], bounds.minimum)
        const next = Math.max(bounds.minimum, Math.min(bounds.maximum, current + distance))
        if (Math.abs(next - current) < 0.01)
            return false
        viewport[property] = next
        return true
    }

    function isScrollViewport(item) {
        return !!item
            && typeof item.contentX === "number"
            && typeof item.contentY === "number"
            && typeof item.contentWidth === "number"
            && typeof item.contentHeight === "number"
    }

    function deepestViewportAt(item, x, y) {
        if (!item || typeof item.childAt !== "function")
            return null
        let current = item
        let px = x
        let py = y
        let found = null
        for (let i = 0; i < 24; ++i) {
            const child = current.childAt(px, py)
            if (!child || child === current)
                break
            if (typeof current.mapToItem !== "function")
                break
            const mapped = current.mapToItem(child, px, py)
            px = mapped.x
            py = mapped.y
            current = child
            if (isScrollViewport(current) && current !== item)
                found = current
        }
        return found
    }

    function ancestorViewport(item) {
        let current = item ? item.parent : null
        for (let i = 0; i < 32 && current; ++i) {
            if (isScrollViewport(current) && current !== item)
                return current
            if (current.contentItem && isScrollViewport(current.contentItem)
                    && current.contentItem !== item)
                return current.contentItem
            current = current.parent
        }
        return null
    }

    function applyWheel(viewport, event, lineExtent, wheelScrollLines) {
        const pixelX = finite(event.pixelDelta.x, 0)
        const pixelY = finite(event.pixelDelta.y, 0)
        const angleX = finite(event.angleDelta.x, 0)
        const angleY = finite(event.angleDelta.y, 0)
        if (pixelX === 0 && pixelY === 0 && angleX === 0 && angleY === 0)
            return false

        if (typeof viewport.cancelFlick === "function")
            viewport.cancelFlick()

        const horizontalBounds = axisBounds(viewport, true)
        const verticalBounds = axisBounds(viewport, false)
        const hasPixelDelta = pixelX !== 0 || pixelY !== 0
        let distanceX = hasPixelDelta
                      ? -pixelX
                      : angleDistance(angleX, lineExtent, wheelScrollLines, horizontalBounds.extent)
        let distanceY = hasPixelDelta
                      ? -pixelY
                      : angleDistance(angleY, lineExtent, wheelScrollLines, verticalBounds.extent)

        // Each axis takes only its own input, even when the other cannot
        // scroll. Shift+wheel asks for horizontal.
        if ((event.modifiers & Qt.ShiftModifier) && distanceX === 0) {
            distanceX = distanceY
            distanceY = 0
        }

        const movedX = moveAxis(viewport, true, distanceX, horizontalBounds)
        const movedY = moveAxis(viewport, false, distanceY, verticalBounds)
        return movedX || movedY
    }

    function handleWheel(viewport, event, lineExtent, wheelScrollLines, coordinateItem) {
        if (!viewport || !event)
            return false

        const origin = coordinateItem || viewport
        const pos = event.position
        let localX = pos ? finite(pos.x, 0) : finite(event.x, 0)
        let localY = pos ? finite(pos.y, 0) : finite(event.y, 0)
        if (origin && viewport && origin !== viewport
                && typeof origin.mapToItem === "function") {
            const mapped = origin.mapToItem(viewport, localX, localY)
            localX = mapped.x
            localY = mapped.y
        }

        // The nested viewport under the pointer owns the gesture. Its handler
        // applies movement and, at a bound, chains to ancestors.
        if (deepestViewportAt(viewport, localX, localY))
            return false

        let current = viewport
        let extent = lineExtent
        while (current) {
            if (applyWheel(current, event, extent, wheelScrollLines))
                return true
            current = ancestorViewport(current)
            if (current && typeof current.wheelLineExtent === "number")
                extent = current.wheelLineExtent
        }
        return false
    }
}
