#include "remote/remote_file_model.h"

#include "remote/remote_constants.h"
#include "remote/remote_shell_channel.h"
#include "remote/remote_transfer_queue.h"
#include "remote/ssh_askpass.h"

#include <QClipboard>
#include <QDateTime>
#include <QFileInfo>
#include <QGuiApplication>
#include <QLocale>

#include <algorithm>
#include <utility>

namespace span::remote {

RemoteFileModel::RemoteFileModel(QObject *parent)
    : QAbstractListModel(parent),
      m_channel(new RemoteShellChannel(this)),
      m_transfers(new RemoteTransferQueue(this))
{
    connect(m_channel, &RemoteShellChannel::replied, this, &RemoteFileModel::handleReply);
    connect(m_channel, &RemoteShellChannel::requestFailed, this,
            &RemoteFileModel::handleFailure);
    connect(m_channel, &RemoteShellChannel::stateChanged, this,
            &RemoteFileModel::connectionStateChanged);
    connect(m_transfers, &RemoteTransferQueue::transferFinished, this,
            [this](RemoteTransferQueue::Direction direction, const QString &destination,
                   bool succeeded, const QString &message) {
                if (!succeeded) {
                    setError(message);
                    return;
                }
                const bool upload = direction == RemoteTransferQueue::Direction::Upload;
                if (upload && destination == m_currentPath) {
                    refresh();
                }
                emit operationCompleted(upload ? tr("Uploaded to %1").arg(destination)
                                               : tr("Downloaded to %1").arg(destination));
            });
}

RemoteFileModel::~RemoteFileModel() = default;

int RemoteFileModel::rowCount(const QModelIndex &parent) const
{
    return parent.isValid() ? 0 : int(m_rows.size());
}

QVariant RemoteFileModel::data(const QModelIndex &index, int role) const
{
    if (!index.isValid() || index.row() < 0 || index.row() >= m_rows.size()) {
        return {};
    }
    const RemoteFileEntry &entry = m_rows.at(index.row());
    switch (role) {
    case NameRole:
        return entry.name;
    case PathRole:
        return entry.path;
    case IsDirRole:
        return entry.isDir;
    case IsLinkRole:
        return entry.isLink;
    case SizeTextRole:
        return entry.isDir ? QString() : QLocale().formattedDataSize(entry.size);
    case ModifiedTextRole:
        return QDateTime::fromSecsSinceEpoch(entry.modifiedSecs)
            .toString(QStringLiteral("yyyy-MM-dd HH:mm"));
    case PermissionsRole:
        return entry.permissions;
    case OwnerRole:
        return entry.owner;
    default:
        return {};
    }
}

QHash<int, QByteArray> RemoteFileModel::roleNames() const
{
    return {
        {NameRole, "name"},
        {PathRole, "path"},
        {IsDirRole, "isDir"},
        {IsLinkRole, "isLink"},
        {SizeTextRole, "sizeText"},
        {ModifiedTextRole, "modifiedText"},
        {PermissionsRole, "permissions"},
        {OwnerRole, "owner"},
    };
}

void RemoteFileModel::configure(const SshEndpoint &endpoint,
                                const QHash<QString, QString> &environment,
                                bool keyChanged)
{
    const QStringList shellArguments = sshShellArguments(endpoint);
    const bool switched = !m_shellArguments.isEmpty()
        && (keyChanged || shellArguments != m_shellArguments);
    m_shellArguments = shellArguments;
    m_promptBridge = environment.contains(QString::fromLatin1(kAskpassPipeEnv));
    m_channel->setLaunch(sshExecutable(), shellArguments, environment);
    m_transfers->setLaunch(sftpExecutable(), sftpBatchArguments(endpoint), environment);
    if (!switched) {
        return;
    }
    // Another host (or user/port/key): nothing listed so far applies to it.
    // Queued transfers stay bound to the old endpoint by the queue itself.
    const bool wasActivated = m_activated;
    m_requests.clear();
    ++m_listGeneration;
    m_channel->stop();
    beginResetModel();
    m_entries.clear();
    m_rows.clear();
    endResetModel();
    emit countChanged();
    m_homePath.clear();
    m_pendingPath.clear();
    if (!m_currentPath.isEmpty()) {
        m_currentPath.clear();
        emit currentPathChanged();
    }
    m_activated = false;
    setLoading(false);
    clearError();
    if (wasActivated) {
        activate();
    }
}

QString RemoteFileModel::connectionState() const
{
    switch (m_channel->state()) {
    case RemoteShellChannel::State::Idle:
        return QStringLiteral("idle");
    case RemoteShellChannel::State::Connecting:
        return QStringLiteral("connecting");
    case RemoteShellChannel::State::Ready:
        return QStringLiteral("ready");
    case RemoteShellChannel::State::Failed:
        return QStringLiteral("failed");
    }
    return {};
}

void RemoteFileModel::setShowHidden(bool show)
{
    if (m_showHidden == show) {
        return;
    }
    m_showHidden = show;
    emit showHiddenChanged();
    rebuildRows();
}

void RemoteFileModel::activate()
{
    if (m_activated) {
        return;
    }
    m_activated = true;
    setLoading(true);
    const quint64 id = m_channel->submit(remoteHomeScript());
    m_requests.insert(id, Request{RequestKind::Home, 0, {}, {}});
}

void RemoteFileModel::openPath(const QString &path)
{
    QString target = path.trimmed();
    if (target.isEmpty()) {
        return;
    }
    if (target == QLatin1String("~") || target.startsWith(QStringLiteral("~/"))) {
        target = m_homePath + target.mid(1);
    } else if (!target.startsWith(QLatin1Char('/'))) {
        target = remoteChildPath(m_currentPath.isEmpty() ? m_homePath : m_currentPath, target);
    }
    if (!m_activated || m_homePath.isEmpty()) {
        m_pendingPath = normalizeRemotePath(target);
        activate();
        return;
    }
    clearError();
    list(normalizeRemotePath(target));
}

void RemoteFileModel::openRow(int row)
{
    if (row >= 0 && row < m_rows.size() && m_rows.at(row).isDir) {
        clearError();
        list(m_rows.at(row).path);
    }
}

void RemoteFileModel::goUp()
{
    if (!m_currentPath.isEmpty() && m_currentPath != QLatin1String("/")) {
        clearError();
        list(remoteParentPath(m_currentPath));
    }
}

void RemoteFileModel::goHome()
{
    if (m_homePath.isEmpty()) {
        activate();
        return;
    }
    clearError();
    list(m_homePath);
}

void RemoteFileModel::refresh()
{
    if (m_currentPath.isEmpty()) {
        if (m_activated && m_channel->state() == RemoteShellChannel::State::Failed) {
            reconnect();
            return;
        }
        activate();
        return;
    }
    list(m_currentPath);
}

void RemoteFileModel::reconnect()
{
    m_channel->stop();
    clearError();
    if (m_homePath.isEmpty()) {
        m_activated = false;
        if (!m_currentPath.isEmpty()) {
            m_pendingPath = m_currentPath;
        }
        activate();
        return;
    }
    list(m_currentPath.isEmpty() ? m_homePath : m_currentPath);
}

void RemoteFileModel::sortBy(const QString &key)
{
    if (key != QLatin1String("name") && key != QLatin1String("size")
        && key != QLatin1String("modified")) {
        return;
    }
    if (m_sortKey == key) {
        m_sortAscending = !m_sortAscending;
    } else {
        m_sortKey = key;
        m_sortAscending = true;
    }
    emit sortChanged();
    rebuildRows();
}

void RemoteFileModel::createFolder(const QString &name)
{
    if (!isValidRemoteName(name) || m_currentPath.isEmpty()) {
        setError(tr("“%1” is not a valid name.").arg(name));
        return;
    }
    const QString path = remoteChildPath(m_currentPath, name);
    submitMutation(remoteMakeDirectoryScript(path), name, {});
}

void RemoteFileModel::createFile(const QString &name)
{
    if (!isValidRemoteName(name) || m_currentPath.isEmpty()) {
        setError(tr("“%1” is not a valid name.").arg(name));
        return;
    }
    const QString path = remoteChildPath(m_currentPath, name);
    submitMutation(remoteCreateFileScript(path), name, {});
}

void RemoteFileModel::rename(const QString &path, const QString &newName)
{
    if (!isValidRemoteName(newName) || !isRemovableRemotePath(path)) {
        setError(tr("“%1” is not a valid name.").arg(newName));
        return;
    }
    const QString target = remoteChildPath(remoteParentPath(path), newName);
    if (target == path) {
        return;
    }
    submitMutation(remoteRenameScript(path, target), newName, {});
}

void RemoteFileModel::remove(const QStringList &paths)
{
    if (paths.isEmpty()) {
        return;
    }
    for (const QString &path : paths) {
        if (!isRemovableRemotePath(path)) {
            setError(tr("Refusing to delete “%1”.").arg(path));
            return;
        }
    }
    submitMutation(remoteRemoveScript(paths), QFileInfo(paths.constFirst()).fileName(), {});
}

void RemoteFileModel::upload(const QVariantList &localUrls)
{
    if (m_currentPath.isEmpty()) {
        return;
    }
    QStringList sources;
    for (const QVariant &value : localUrls) {
        const QUrl url = value.toUrl();
        const QString path = url.isLocalFile() ? url.toLocalFile() : value.toString();
        if (!path.isEmpty() && QFileInfo::exists(path)) {
            sources << path;
        }
    }
    m_transfers->enqueue(RemoteTransferQueue::Direction::Upload, sources, m_currentPath);
}

void RemoteFileModel::download(const QStringList &remotePaths, const QUrl &localFolder)
{
    const QString folder = localFolder.toLocalFile();
    if (folder.isEmpty() || !QFileInfo(folder).isDir()) {
        setError(tr("Choose an existing local folder."));
        return;
    }
    QStringList sources;
    for (const QString &path : remotePaths) {
        if (isRemovableRemotePath(path)) {
            sources << path;
        }
    }
    m_transfers->enqueue(RemoteTransferQueue::Direction::Download, sources, folder);
}

void RemoteFileModel::copyPathToClipboard(const QString &path) const
{
    if (QClipboard *clipboard = QGuiApplication::clipboard()) {
        clipboard->setText(path);
    }
}

void RemoteFileModel::clearError()
{
    setError(QString());
}

void RemoteFileModel::list(const QString &path)
{
    const quint64 generation = ++m_listGeneration;
    setLoading(true);
    const quint64 id = m_channel->submit(remoteListScript(path));
    m_requests.insert(id, Request{RequestKind::List, generation, path, {}});
}

void RemoteFileModel::submitMutation(const QString &script, const QString &target,
                                     const QString &notice)
{
    const quint64 id = m_channel->submit(script);
    m_requests.insert(id, Request{RequestKind::Mutation, 0, target, notice});
}

void RemoteFileModel::handleReply(quint64 id, int exitCode, const QByteArray &payload)
{
    const auto it = m_requests.constFind(id);
    if (it == m_requests.cend()) {
        return;
    }
    const Request request = it.value();
    m_requests.erase(it);
    switch (request.kind) {
    case RequestKind::Home: {
        const QString home = QString::fromUtf8(payload).trimmed();
        m_homePath = home.startsWith(QLatin1Char('/')) ? normalizeRemotePath(home)
                                                        : QStringLiteral("/");
        list(m_pendingPath.isEmpty() ? m_homePath : std::exchange(m_pendingPath, {}));
        return;
    }
    case RequestKind::List: {
        if (request.generation != m_listGeneration) {
            return;
        }
        setLoading(false);
        // Any failure leaves the current directory and its rows shown. The
        // output then is the shell's or find's error, after the directory
        // record when the directory could be entered.
        RemoteListing listing;
        if (exitCode == kListMissingFindExitCode) {
            setError(tr("Cannot list %1. The remote host needs GNU find.").arg(request.path));
            return;
        }
        if (exitCode != 0 || !parseRemoteListing(payload, &listing)) {
            const qsizetype record = payload.indexOf('\0');
            const QString detail =
                QString::fromUtf8(record < 0 ? payload : payload.mid(record + 1)).trimmed();
            setError(detail.isEmpty() ? tr("Cannot open %1.").arg(request.path) : detail);
            return;
        }
        applyListing(listing);
        return;
    }
    case RequestKind::Mutation:
        if (exitCode == 0) {
            if (!request.notice.isEmpty()) {
                emit operationCompleted(request.notice);
            }
        } else if (exitCode == kRemoteExistsExitCode) {
            setError(tr("“%1” already exists.").arg(request.path));
        } else {
            const QString detail = QString::fromUtf8(payload).trimmed();
            setError(detail.isEmpty() ? tr("The remote operation on “%1” failed.").arg(request.path)
                                      : detail);
        }
        refresh();
        return;
    }
}

void RemoteFileModel::handleFailure(quint64 id, const QString &message)
{
    const Request request = m_requests.take(id);
    if (request.kind != RequestKind::Mutation
        && (request.kind == RequestKind::Home || request.generation == m_listGeneration)) {
        setLoading(false);
    }
    if (message.isEmpty()) {
        return;
    }
    // ssh started without the bridge cannot ask anything: a password, key
    // passphrase or unknown host key ends the connection with ssh's own
    // terse error, which does not say why.
    setError(m_promptBridge
                 ? message
                 : message + QLatin1Char('\n')
                       + tr("SpanTui's SSH prompt bridge is not running, so the file browser cannot "
                            "answer password, key passphrase or host key questions. Use key or "
                            "agent login, answer the host key once in a remote terminal, or "
                            "restart SpanTui."));
}

void RemoteFileModel::applyListing(const RemoteListing &listing)
{
    m_entries = listing.entries;
    if (m_currentPath != listing.directory) {
        m_currentPath = listing.directory;
        emit currentPathChanged();
    }
    rebuildRows();
}

void RemoteFileModel::rebuildRows()
{
    QVector<RemoteFileEntry> rows;
    rows.reserve(m_entries.size());
    for (const RemoteFileEntry &entry : std::as_const(m_entries)) {
        if (m_showHidden || !entry.name.startsWith(QLatin1Char('.'))) {
            rows.push_back(entry);
        }
    }
    const QString key = m_sortKey;
    const bool ascending = m_sortAscending;
    std::stable_sort(rows.begin(), rows.end(),
                     [&key, ascending](const RemoteFileEntry &a, const RemoteFileEntry &b) {
                         // Folders stay on top in either direction.
                         if (a.isDir != b.isDir) {
                             return a.isDir;
                         }
                         int order = 0;
                         if (key == QLatin1String("size")) {
                             order = a.size < b.size ? -1 : (a.size > b.size ? 1 : 0);
                         } else if (key == QLatin1String("modified")) {
                             order = a.modifiedSecs < b.modifiedSecs
                                 ? -1 : (a.modifiedSecs > b.modifiedSecs ? 1 : 0);
                         }
                         if (order == 0) {
                             order = a.name.compare(b.name, Qt::CaseInsensitive);
                         }
                         return ascending ? order < 0 : order > 0;
                     });
    const int oldCount = int(m_rows.size());
    beginResetModel();
    m_rows = std::move(rows);
    endResetModel();
    if (oldCount != m_rows.size()) {
        emit countChanged();
    }
}

void RemoteFileModel::setLoading(bool loading)
{
    if (m_loading == loading) {
        return;
    }
    m_loading = loading;
    emit loadingChanged();
}

void RemoteFileModel::setError(const QString &message)
{
    if (m_errorMessage == message) {
        return;
    }
    m_errorMessage = message;
    emit errorMessageChanged();
}

} // namespace span::remote
