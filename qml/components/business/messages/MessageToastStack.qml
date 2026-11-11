pragma ComponentBehavior: Bound

import QtQuick
import SpanTui

Item {
    id: root

    readonly property real renderDpr: PixelGrid.safeDpr(Screen.devicePixelRatio)
    readonly property bool stackEnabled: !MessageCenter.inboxOpen
                                         && MessageCenter.toasts.length > 0

    function visualIndex(messageId) {
        for (let i = 0; i < visualToasts.count; ++i) {
            if (visualToasts.get(i).messageId === messageId)
                return i
        }
        return -1
    }

    function syncToasts() {
        const incoming = MessageCenter.toasts
        const incomingIds = {}
        for (let i = 0; i < incoming.length; ++i) {
            const toast = incoming[i]
            const messageId = Number(toast.id)
            incomingIds[String(messageId)] = true
            const existing = visualIndex(messageId)
            if (existing < 0) {
                visualToasts.append({
                    messageId: messageId,
                    messageKind: toast.kind,
                    messageTitle: toast.title,
                    messageBody: toast.body,
                    presented: true
                })
            } else {
                visualToasts.setProperty(existing, "presented", true)
            }
        }
        for (let i = visualToasts.count - 1; i >= 0; --i) {
            const localId = visualToasts.get(i).messageId
            if (!incomingIds[String(localId)])
                visualToasts.setProperty(i, "presented", false)
        }
    }

    function removeVisualToast(messageId) {
        const visual = visualIndex(messageId)
        if (visual >= 0 && !visualToasts.get(visual).presented)
            visualToasts.remove(visual)
    }

    implicitWidth: PixelGrid.snapSize(320, renderDpr)
    implicitHeight: stack.implicitHeight
    width: implicitWidth
    height: implicitHeight
    visible: visualToasts.count > 0
    opacity: MessageCenter.inboxOpen ? 0 : 1
    enabled: stackEnabled

    Behavior on opacity {
        enabled: !Appearance.reduceMotion
        NumberAnimation { duration: Theme.durFast; easing.type: Easing.OutCubic }
    }

    Component.onCompleted: syncToasts()

    Connections {
        target: MessageCenter
        function onToastsChanged() {
            root.syncToasts()
        }
    }

    ListModel {
        id: visualToasts
    }

    Column {
        id: stack
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: parent.top
        spacing: Theme.spaceMd

        Repeater {
            model: visualToasts

            MessageCard {
                required property double messageId
                required property string messageKind
                required property string messageTitle
                required property string messageBody
                required property bool presented

                width: stack.width
                kind: messageKind
                title: messageTitle
                body: messageBody
                shown: presented
                onDismissed: MessageCenter.dismissToast(messageId)
                onExitFinished: root.removeVisualToast(messageId)
            }
        }
    }
}
