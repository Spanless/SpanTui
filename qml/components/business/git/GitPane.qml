import QtQuick
import QtQuick.Layouts
import SpanTui

Item {
    id: root
    objectName: "gitPane"
    required property GitRepository repository
    readonly property GitRepository gitRepository: repository

    // "diff" | "history" — owned by the dock's segmented switch.
    property string section: "diff"

    ColumnLayout {
        anchors.fill: parent
        spacing: 0

        StackLayout {
            Layout.fillWidth: true
            Layout.fillHeight: true
            visible: root.gitRepository !== null
            // Persistent pages: swapping bodies through a Loader destroys the
            // old page and can abort in-flight delegate incubation, which
            // surfaces "Cannot create delegate" warnings as message toasts.
            currentIndex: root.section === "history" ? 1 : 0

            GitCommitDiffList {
                repository: root.gitRepository
                scope: "changes"
                model: root.gitRepository ? root.gitRepository.commitChanges : null
                loading: Boolean(root.gitRepository && root.gitRepository.changesViewLoading)
                emptyText: qsTr("No changed files")
            }

            ColumnLayout {
                spacing: 0

                GitLogView {
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    Layout.minimumHeight: 96   // style-ok: history section min height
                    repository: root.gitRepository
                }

                PixelLine {
                    Layout.fillWidth: true
                    Layout.preferredHeight: implicitHeight
                    lineColor: Theme.separator
                }

                Item {
                    Layout.fillWidth: true
                    Layout.preferredHeight: Theme.sizeMd
                    visible: Boolean(root.gitRepository
                                     && root.gitRepository.selectedCommit
                                     && root.gitRepository.selectedCommit.length > 0)
                    UiText {
                        anchors.fill: parent
                        anchors.leftMargin: Theme.spaceMd
                        anchors.rightMargin: Theme.spaceMd
                        text: (root.gitRepository
                               && root.gitRepository.selectedCommitDetails
                               && root.gitRepository.selectedCommitDetails.subject)
                              ? root.gitRepository.selectedCommitDetails.subject
                              : (root.gitRepository ? root.gitRepository.selectedCommit.substring(0, 8) : "")
                        size: "md"
                        elide: Text.ElideRight
                    }
                }

                GitCommitDiffList {
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    Layout.minimumHeight: 120  // style-ok: commit diff section min height
                    objectName: "commitDiffListPane"
                    repository: root.gitRepository
                    scope: "commit"
                    model: root.gitRepository ? root.gitRepository.commitDiffFiles : null
                    loading: Boolean(root.gitRepository && root.gitRepository.commitViewLoading)
                    allowWorkspaceOpen: false
                    emptyText: (root.gitRepository
                                && root.gitRepository.selectedCommit
                                && root.gitRepository.selectedCommit.length > 0)
                               ? qsTr("No changed files")
                               : qsTr("Select a commit")
                }
            }
        }

        EmptyState {
            Layout.fillWidth: true
            Layout.fillHeight: true
            active: root.gitRepository === null
            title: qsTr("Focus a terminal to browse Git")
        }
    }
}
