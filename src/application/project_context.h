#pragma once

#include <QObject>
#include <QHash>
#include <QPointer>
#include "documents/document_manager.h"
#include "git/git_repository_service.h"
#include "workspace/file_tree_model.h"

namespace span::application {

class ProjectContext final : public QObject {
    Q_OBJECT
    Q_PROPERTY(QString projectPath READ projectPath CONSTANT FINAL)
    Q_PROPERTY(FileTreeModel *fileTree READ fileTree CONSTANT FINAL)
    Q_PROPERTY(GitRepositoryService *gitRepository READ gitRepository CONSTANT FINAL)

public:
    ProjectContext(QString projectPath, QString databasePath,
                   QObject *parent = nullptr);

    QString projectPath() const;
    FileTreeModel *fileTree() const;
    GitRepositoryService *gitRepository() const;
    void setIdle(bool idle);
    // A project no tile or dock page shows keeps its Git state but stops
    // polling; WorkspaceController decides which projects are foreground.
    void setForeground(bool foreground);
    // Git runs only while enabled (the tool registry found a usable Git).
    // Disabling clears the repository state and cancels running commands;
    // re-enabling discovers the repository again. Starts disabled.
    void setGitEnabled(bool enabled);

    Q_INVOKABLE DocumentManager *editorDocuments(const QString &tileId);
    Q_INVOKABLE DocumentManager *existingEditorDocuments(const QString &tileId) const;
    Q_INVOKABLE void releaseEditor(const QString &tileId);
    QString editorTileForFile(const QString &canonicalPath) const;
signals:
    void editorDocumentsCreated(DocumentManager *documents);

private:
    QString m_projectPath;
    QString m_databasePath;
    FileTreeModel *m_fileTree = nullptr;
    GitRepositoryService *m_gitRepository = nullptr;
    bool m_gitEnabled = false;
    QHash<QString, QPointer<DocumentManager>> m_editors;
};

} // namespace span::application
