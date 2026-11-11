import QtQuick
import QtQuick.Controls
import SpanTui

// Shared context menu: point-positioned drop panel over AppPopup.
// While open it marks its target: the nearest ancestor-or-self of the source
// item that declares `menuTarget` (ListItem) gets it raised, so the row the
// menu acts on is visible without touching the selection. Callers pass the
// row (or its mouseArea) as the source — no other wiring.
//
// An action with a `submenu` array opens it as a second panel beside its row
// (one level deep): on hover after a short intent delay, or at once on click.
// Crossing other rows on the way into the panel does not close it unless the
// pointer rests on one. An outside press closes the whole menu.
AppPopup {
    id: root

    property var actions: []
    // Guarded object reference: nulls itself if the row is destroyed.
    property Item _targetRow: null
    // Row whose submenu is open; the pending pair waits for the intent delay.
    property Item _submenuRow: null
    property Item _pendingRow: null
    property var _pendingAction: null
    // This menu closed the submenu itself (pointer rested on a plain row,
    // menu reopened or closing); its `closed` must not close the menu.
    property bool _dismissingSubmenu: false

    function openAt(sourceItem, localX, localY, menuActions) {
        _closeSubmenu()
        actions = menuActions
        placeAt(sourceItem, localX, localY)
    }

    function _hasSubmenu(action) {
        return action && Array.isArray(action.submenu) && action.submenu.length > 0
    }

    function _openSubmenu(action, row) {
        hoverIntent.stop()
        if (submenu.opened && _submenuRow === row)
            return
        // Switching rows moves the open panel instead of closing it.
        _dismissingSubmenu = false
        _submenuRow = row
        submenu.actions = action.submenu
        // Beside the panel edge, or mirrored to the left when it would not
        // fit; the first submenu row lines up with the hovered row.
        const overlay = Overlay.overlay
        const gap = root.padding
        const rightX = row.width + root.padding + gap
        const fitsRight = row.mapToItem(overlay, rightX, 0).x + submenu.implicitWidth
                          <= overlay.width - submenu.edgeMargin
        submenu.placeAt(row, fitsRight ? rightX : -(root.padding + gap + submenu.implicitWidth),
                        -submenu.padding)
    }

    function _closeSubmenu() {
        hoverIntent.stop()
        _pendingRow = null
        _pendingAction = null
        if (!submenu.visible)
            return
        _dismissingSubmenu = true
        submenu.close()
    }

    function _markTarget(item) {
        _clearTarget()
        let node = item
        while (node) {
            // Bracket access: the property is absent on intermediate items.
            if (node["menuTarget"] !== undefined) {
                node.menuTarget = true
                _targetRow = node
                return
            }
            node = node.parent
        }
    }

    function _clearTarget() {
        if (_targetRow)
            _targetRow.menuTarget = false
        _targetRow = null
    }

    onOpening: source => _markTarget(source)
    onPopupClosed: {
        _closeSubmenu()
        _clearTarget()
    }

    // radiusLg surface − spaceSm padding = radiusSm rows: concentric.
    padding: Theme.spaceSm

    contentItem: ActionList {
        actions: root.actions
        revealed: root.visible
        onTriggered: (action, row) => {
            if (root._hasSubmenu(action)) {
                root._openSubmenu(action, row)
                return
            }
            root.close()
            if (action && action.action)
                action.action()
        }
        onRowHovered: (action, row) => {
            if (row === root._submenuRow && submenu.opened) {
                hoverIntent.stop()
                return
            }
            root._pendingAction = action
            root._pendingRow = row
            hoverIntent.restart()
        }

        HoverHandler { id: menuHover }
    }

    // Hover intent: the pointer must rest on a row before the submenu
    // switches, so a diagonal move toward the open panel can cross rows.
    readonly property Timer hoverIntentTimer: Timer {
        id: hoverIntent
        interval: Theme.durMid
        onTriggered: {
            const action = root._pendingAction
            const row = root._pendingRow
            if (root._hasSubmenu(action) && row)
                root._openSubmenu(action, row)
            else
                root._closeSubmenu()
        }
    }

    readonly property AppPopup submenuPopup: AppPopup {
        id: submenu

        property var actions: []

        padding: Theme.spaceSm

        // Only the top popup closes on an outside press; one that also
        // missed this menu closes the whole chain.
        onPopupClosed: {
            const dismissed = root._dismissingSubmenu
            root._dismissingSubmenu = false
            root._submenuRow = null
            if (!dismissed && !menuHover.hovered)
                root.close()
        }

        contentItem: ActionList {
            actions: submenu.actions
            revealed: submenu.visible
            onRowHovered: hoverIntent.stop()
            onTriggered: action => {
                root._closeSubmenu()
                root.close()
                if (action && action.action)
                    action.action()
            }
        }
    }
}
