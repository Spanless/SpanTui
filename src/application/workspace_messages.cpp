#include "application/workspace_messages.h"

#include "application/message_center.h"
#include "documents/document_manager.h"
#include "git/git_repository_service.h"
#include "remote/remote_file_model.h"
#include "workspace/file_tree_model.h"

namespace span::application {

namespace {

void post(MessageKind kind, const QString &message)
{
    if (auto *center = MessageCenter::instance()) {
        center->post(kind, message);
    }
}

void postError(const QString &message)
{
    if (!message.isEmpty()) {
        post(MessageKind::Error, message);
    }
}

void postSuccess(const QString &message)
{
    post(MessageKind::Success, message);
}

} // namespace

void connectFileTreeMessages(FileTreeModel *files, QObject *lifetime)
{
    QObject::connect(files, &FileTreeModel::errorMessageChanged,
                     lifetime, [files]() { postError(files->errorMessage()); });
    QObject::connect(files, &FileTreeModel::operationCompleted, lifetime, postSuccess);
}

void connectGitMessages(GitRepositoryService *git, QObject *lifetime)
{
    QObject::connect(git, &GitRepositoryService::errorMessageChanged,
                     lifetime, [git]() { postError(git->errorMessage()); });
}

void connectDocumentMessages(DocumentManager *documents)
{
    QObject::connect(documents, &DocumentManager::errorOccurred, documents, postError);
}

void connectRemoteFileMessages(span::remote::RemoteFileModel *files, QObject *lifetime)
{
    QObject::connect(files, &span::remote::RemoteFileModel::errorMessageChanged,
                     lifetime, [files]() { postError(files->errorMessage()); });
    QObject::connect(files, &span::remote::RemoteFileModel::operationCompleted,
                     lifetime, postSuccess);
}

} // namespace span::application
