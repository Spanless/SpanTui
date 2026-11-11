pragma Translator: "GitHistoryPane"
pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import SpanTui

// Git reference tree of the title-bar branch popup: left-clicking a row
// copies the reference name and reports it through referenceActivated.
Item {
    id: root
    required property GitRepository repository
    readonly property GitRepository gitRepository: repository

    property string query: ""
    readonly property bool filtering: query.trim().length > 0
    property bool localExpanded: true
    property bool remoteExpanded: true
    property bool tagExpanded: true
    property var remoteGroupExpansion: ({})

    signal referenceActivated(var reference)

    // Rows climb past refsColumn to this root for the glide context —
    // declared, not injected: QML cannot add dynamic properties at runtime.
    property var glideContext: refsGlide.glideEnabled ? refsGlide : null

    // GitItemModel updates rows in place via dataChanged. The derived arrays
    // below read itemAt(), which QML cannot observe, so every mutation bumps
    // this revision to force the arrays to rebuild.
    property int branchesRevision: 0
    Connections {
        target: root.gitRepository ? root.gitRepository.branches : null
        ignoreUnknownSignals: true
        function onDataChanged() { root.branchesRevision += 1 }
        function onModelReset() { root.branchesRevision += 1 }
        function onRowsInserted() { root.branchesRevision += 1 }
        function onRowsRemoved() { root.branchesRevision += 1 }
    }

    readonly property real renderDpr: PixelGrid.safeDpr(Screen.devicePixelRatio)
    readonly property var localBranches: {
        const _q = root.query
        const _branches = root.branchesRevision
        const list = []
        const count = (gitRepository && gitRepository.branches) ? gitRepository.branches.count : 0
        for (let i = 0; i < count; ++i) {
            const it = gitRepository.branches.itemAt(i)
            if (it && root.refMatches(it, "local"))
                list.push(it)
        }
        return list
    }

    readonly property var remoteBranches: {
        const _q = root.query
        const _branches = root.branchesRevision
        const list = []
        const count = (gitRepository && gitRepository.branches) ? gitRepository.branches.count : 0
        for (let i = 0; i < count; ++i) {
            const it = gitRepository.branches.itemAt(i)
            if (it && root.refMatches(it, "remote"))
                list.push(it)
        }
        return list
    }

    readonly property var tagBranches: {
        const _q = root.query
        const _branches = root.branchesRevision
        const list = []
        const count = (gitRepository && gitRepository.branches) ? gitRepository.branches.count : 0
        for (let i = 0; i < count; ++i) {
            const it = gitRepository.branches.itemAt(i)
            if (it && root.refMatches(it, "tag"))
                list.push(it)
        }
        return list
    }

    function clearFilterAndFocus() {
        query = ""
        refFilter.text = ""
        refFilter.field.forceActiveFocus()
    }

    function refMatches(item, kind) {
        const normalized = query.trim().toLowerCase()
        return item.kind === kind
            && (normalized.length === 0
                || String(item.name || "").toLowerCase().indexOf(normalized) >= 0)
    }

    function remoteGroupIsExpanded(group) {
        return remoteGroupExpansion[String(group)] !== false
    }

    function toggleRemoteGroup(group) {
        const key = String(group)
        const next = Object.assign({}, remoteGroupExpansion)
        next[key] = !remoteGroupIsExpanded(key)
        remoteGroupExpansion = next
    }

    function remoteLeafName(item) {
        const name = String(item.name || "")
        const group = String(item.group || "")
        const prefix = group + "/"
        return group.length > 0 && name.startsWith(prefix)
                ? name.substring(prefix.length) : name
    }

    function isRefSelected(item) {
        return !!(item && gitRepository && gitRepository.currentBranch === item.name)
    }

    // Right-clicks stay inert; only the left button copies the name.
    function handleRefClick(mouse, item) {
        if (mouse.button === Qt.RightButton || !item || !item.name)
            return
        Clipboard.copyText(item.name)
        referenceActivated(item)
    }

    ColumnLayout {
        anchors.fill: parent
        spacing: 0

        FilterBar {
            id: refFilter
            Layout.fillWidth: true
            placeholderText: qsTr("Filter branches or tags")
            text: root.query
            onEdited: text => root.query = text
        }

        DesktopScrollView {
            id: refsView
            Layout.fillWidth: true
            Layout.fillHeight: true
            clip: true
            // The viewport owns the width. The only host (BranchPopup's list
            // pane) is a rounded popup surface the bar runs into.
            contentWidth: availableWidth
            scrollBarEndInset: Theme.radiusLg
            ScrollBar.horizontal.policy: ScrollBar.AlwaysOff

            Column {
                id: refsColumn
                width: refsView.availableWidth

                TreeGroupHeader {
                    width: parent.width
                    text: qsTr("Local")
                    iconName: "git-branch"
                    expanded: root.localExpanded || root.filtering
                    onToggled: root.localExpanded = !root.localExpanded
                }
                TreeExpandHost {
                    expanded: root.localExpanded || root.filtering
                    hasContent: localRepeater.count > 0
                    Repeater {
                        id: localRepeater
                        model: root.localBranches
                        delegate: ListItem {
                            id: localRefRow
                            required property var modelData
                            readonly property var item: modelData
                            width: refsColumn.width
                            selected: root.isRefSelected(localRefRow.item)
                            onClicked: mouse => root.handleRefClick(
                                           mouse, localRefRow.item)
                            RowLayout {
                                anchors.fill: parent
                                anchors.leftMargin: Theme.treeDepthStep
                                spacing: Theme.spaceSm
                                z: Theme.zRaised
                                Icon { name: "git-branch"; size: Theme.iconMd; color: localRefRow.item.current ? Theme.accent : Theme.muted }
                                UiText { Layout.fillWidth: true; Layout.minimumWidth: 0; text: localRefRow.item.name; size: "md"; elide: Text.ElideMiddle }
                                Icon {
                                    visible: localRefRow.item.current
                                    name: "check"
                                    size: Theme.iconSm
                                    color: Theme.accent
                                    HoverHandler { id: localCurrentHover }
                                    Tip {
                                        visible: localCurrentHover.hovered
                                        text: qsTr("Current branch")
                                    }
                                }
                                UiText { visible: localRefRow.item.ahead > 0; text: "↑" + localRefRow.item.ahead; size: "sm"; color: Theme.gitAdded }
                                UiText { visible: localRefRow.item.behind > 0; text: "↓" + localRefRow.item.behind; size: "sm"; color: Theme.gitModified }
                            }
                        }
                    }
                }

                TreeGroupHeader {
                    width: parent.width
                    text: qsTr("Remote")
                    iconName: "git-remote"
                    expanded: root.remoteExpanded || root.filtering
                    onToggled: root.remoteExpanded = !root.remoteExpanded
                }
                TreeExpandHost {
                    expanded: root.remoteExpanded || root.filtering
                    hasContent: remoteRepeater.count > 0
                    Repeater {
                        id: remoteRepeater
                        model: root.remoteBranches
                        delegate: Column {
                            id: remoteRefRow
                            required property int index
                            required property var modelData
                            readonly property var item: modelData
                            readonly property bool firstInGroup: {
                                if (index === 0)
                                    return true
                                const prev = remoteRepeater.model[index - 1]
                                return !prev || String(prev.group) !== String(item.group)
                            }
                            readonly property bool groupOpen: root.filtering
                                || root.remoteGroupIsExpanded(item.group)
                            width: refsColumn.width
                            TreeGroupHeader {
                                width: parent.width
                                visible: remoteRefRow.firstInGroup
                                height: visible ? Theme.sizeMd : 0
                                text: String(remoteRefRow.item.group || qsTr("Remote"))
                                iconName: "folder"
                                depth: 1
                                expanded: remoteRefRow.groupOpen
                                onToggled: root.toggleRemoteGroup(remoteRefRow.item.group)
                            }
                            TreeExpandHost {
                                expanded: remoteRefRow.groupOpen
                                ListItem {
                                    width: parent.width
                                    selected: root.isRefSelected(remoteRefRow.item)
                                    onClicked: mouse => root.handleRefClick(
                                                   mouse, remoteRefRow.item)
                                    RowLayout {
                                        anchors.fill: parent
                                        anchors.leftMargin: Theme.treeDepthStep * 2
                                        spacing: Theme.spaceSm
                                        z: Theme.zRaised
                                        Icon { name: "git-remote"; size: Theme.iconMd; color: Theme.gitRenamed }
                                        UiText { Layout.fillWidth: true; Layout.minimumWidth: 0; text: root.remoteLeafName(remoteRefRow.item); size: "md"; elide: Text.ElideMiddle }
                                    }
                                }
                            }
                        }
                    }
                }

                TreeGroupHeader {
                    width: parent.width
                    text: qsTr("Tags")
                    iconName: "tag"
                    expanded: root.tagExpanded || root.filtering
                    onToggled: root.tagExpanded = !root.tagExpanded
                }
                TreeExpandHost {
                    expanded: root.tagExpanded || root.filtering
                    hasContent: tagRepeater.count > 0
                    Repeater {
                        id: tagRepeater
                        model: root.tagBranches
                        delegate: ListItem {
                            id: tagRefRow
                            required property var modelData
                            readonly property var item: modelData
                            width: refsColumn.width
                            selected: root.isRefSelected(tagRefRow.item)
                            onClicked: mouse => root.handleRefClick(
                                           mouse, tagRefRow.item)
                            RowLayout {
                                anchors.fill: parent
                                anchors.leftMargin: Theme.treeDepthStep
                                spacing: Theme.spaceSm
                                z: Theme.zRaised
                                Icon { name: "tag"; size: Theme.iconMd; color: Theme.gitRenamed }
                                UiText { Layout.fillWidth: true; Layout.minimumWidth: 0; text: tagRefRow.item.name; size: "md"; elide: Text.ElideMiddle }
                            }
                        }
                    }
                }
            }

            // Shared hover pill riding the scroll content; rows climb to
            // this root for the declared glideContext forwarder.
            GlideHighlight {
                id: refsGlide
                parent: refsColumn.parent
                target: refsColumn
                fillColor: Theme.hover
                insetX: Theme.spaceSm
                insetY: PixelGrid.hairline(root.renderDpr)
                scrollOffset: refsView.contentItem
                              ? refsView.contentItem.contentY : 0
            }
        }
    }
}
