#include "application/project_context.h"

#include "documents/document_manager.h"
#include "git/git_repository_service.h"
#include "workspace/file_tree_model.h"

#include <QFileInfo>

namespace span::application {

ProjectContext::ProjectContext(QString projectPath, QString databasePath,
                               QObject *parent)
    : QObject(parent),
      m_projectPath(std::move(projectPath)),
      m_databasePath(std::move(databasePath)),
      m_fileTree(new FileTreeModel(this)),
      m_gitRepository(new GitRepositoryService(this))
{
    m_fileTree->setRootPath(m_projectPath);
    connect(m_gitRepository, &GitRepositoryService::statusMapChanged,
            m_fileTree, [this]() {
        m_fileTree->setGitStatusMap(m_gitRepository->statusMap(),
                                    m_gitRepository->repositoryRoot());
    });
    connect(m_gitRepository, &GitRepositoryService::repositoryChanged,
            m_fileTree, [this]() {
        m_fileTree->setGitStatusMap(m_gitRepository->statusMap(),
                                    m_gitRepository->repositoryRoot());
    });
    connect(m_fileTree, &FileTreeModel::pathRenamed, this,
            [this](const QString &oldPath, const QString &newPath) {
        for (const auto &documents : m_editors) {
            if (documents) {
                documents->updatePath(oldPath, newPath);
            }
        }
    });
    connect(m_fileTree, &FileTreeModel::pathDeleted, this,
            [this](const QString &path) {
        for (const auto &documents : m_editors) {
            if (documents) {
                documents->markPathDeleted(path);
            }
        }
    });
    connect(m_fileTree, &FileTreeModel::pathRestored, this,
            [this](const QString &path) {
        for (const auto &documents : m_editors) {
            if (documents) {
                documents->refreshPath(path);
            }
        }
    });
}

QString ProjectContext::projectPath() const { return m_projectPath; }
FileTreeModel *ProjectContext::fileTree() const { return m_fileTree; }
GitRepositoryService *ProjectContext::gitRepository() const { return m_gitRepository; }
void ProjectContext::setIdle(bool idle) { m_gitRepository->setIdle(idle); }
void ProjectContext::setForeground(bool foreground)
{
    m_gitRepository->setForeground(foreground);
}

void ProjectContext::setGitEnabled(bool enabled)
{
    if (m_gitEnabled == enabled) {
        return;
    }
    m_gitEnabled = enabled;
    m_gitRepository->setProjectPath(enabled ? m_projectPath : QString());
}

DocumentManager *ProjectContext::editorDocuments(const QString &tileId)
{
    if (tileId.isEmpty()) {
        return nullptr;
    }
    if (auto manager = m_editors.value(tileId)) {
        return manager;
    }
    auto *manager = new DocumentManager(this);
    manager->setOwnerScope(m_projectPath, tileId);
    if (!manager->initialize(m_databasePath)) {
        manager->deleteLater();
        return nullptr;
    }
    m_editors.insert(tileId, manager);
    emit editorDocumentsCreated(manager);
    return manager;
}

DocumentManager *ProjectContext::existingEditorDocuments(const QString &tileId) const
{
    return m_editors.value(tileId);
}

void ProjectContext::releaseEditor(const QString &tileId)
{
    if (auto manager = m_editors.take(tileId)) {
        manager->flush();
        manager->deleteLater();
    }
}

QString ProjectContext::editorTileForFile(const QString &canonicalPath) const
{
    const QString wanted = QFileInfo(canonicalPath).canonicalFilePath();
    if (wanted.isEmpty()) {
        return {};
    }
    for (auto it = m_editors.cbegin(); it != m_editors.cend(); ++it) {
        DocumentManager *manager = it.value();
        if (manager && manager->indexForCanonicalPath(wanted) >= 0) {
            return it.key();
        }
    }
    return {};
}

} // namespace span::application
