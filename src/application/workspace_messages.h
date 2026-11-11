#pragma once

class DocumentManager;
class FileTreeModel;
class GitRepositoryService;
class QObject;

namespace span::remote {
class RemoteFileModel;
}

namespace span::application {

// Domain owners report to the MessageCenter on one path each: a non-empty
// error becomes one Error entry (clearing it posts nothing), a finished file
// operation one Success entry. `lifetime` ends the connection, normally the
// project or host context that owns the model.
void connectFileTreeMessages(FileTreeModel *files, QObject *lifetime);
void connectGitMessages(GitRepositoryService *git, QObject *lifetime);
void connectDocumentMessages(DocumentManager *documents);
void connectRemoteFileMessages(span::remote::RemoteFileModel *files, QObject *lifetime);

} // namespace span::application
