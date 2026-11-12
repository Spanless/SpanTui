#include "git/git_repository_service.h"

#include "git/git_constants.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QMap>
#include <QRegularExpression>

#include <algorithm>

namespace {
constexpr int kActivePollMs = 10000;
constexpr int kIdlePollMs = 30000;
constexpr int kRefreshCoalesceMs = 250;
} // namespace

GitItemModel::GitItemModel(QObject *parent) : QAbstractListModel(parent) {}

int GitItemModel::rowCount(const QModelIndex &parent) const {
    return parent.isValid() ? 0 : m_items.size();
}

int GitItemModel::count() const { return m_items.size(); }

QVariant GitItemModel::data(const QModelIndex &index, int role) const {
    if (!index.isValid() || index.row() < 0 || index.row() >= m_items.size() || role != ItemRole)
        return {};
    return m_items.at(index.row());
}

QHash<int, QByteArray> GitItemModel::roleNames() const {
    return {{ItemRole, "item"}};
}

const QVariantList &GitItemModel::items() const { return m_items; }

QVariantMap GitItemModel::itemAt(int row) const {
    if (row < 0 || row >= m_items.size()) return {};
    return m_items.at(row).toMap();
}

QString GitItemModel::identityKey(const QVariantMap &item) {
    const QString path = item.value(QStringLiteral("path")).toString();
    if (!path.isEmpty()) {
        return QLatin1String("p:") + path;
    }
    const QString fullRef = item.value(QStringLiteral("fullRef")).toString();
    if (!fullRef.isEmpty()) {
        return QLatin1String("r:") + fullRef;
    }
    const QString hash = item.value(QStringLiteral("hash")).toString();
    if (!hash.isEmpty()) {
        return QLatin1String("h:") + hash;
    }
    return QString();
}

bool GitItemModel::uniqueIdentities(const QVariantList &items,
                                    QStringList *orderedIds,
                                    QSet<QString> *idSet) {
    if (orderedIds) orderedIds->clear();
    if (idSet) idSet->clear();
    if (orderedIds) orderedIds->reserve(items.size());
    if (idSet) idSet->reserve(items.size());
    QSet<QString> localSet;
    QSet<QString> &seen = idSet ? *idSet : localSet;
    for (const QVariant &itemVal : items) {
        const QString key = identityKey(itemVal.toMap());
        if (key.isEmpty() || seen.contains(key)) {
            return false;
        }
        seen.insert(key);
        if (orderedIds) {
            orderedIds->append(key);
        }
    }
    return true;
}

void GitItemModel::invalidateHashRows()
{
    m_hashRowsValid = false;
    m_hashRows.clear();
}

int GitItemModel::rowOfHash(const QString &hash) const
{
    if (hash.isEmpty()) return -1;
    if (!m_hashRowsValid) {
        m_hashRows.clear();
        m_hashRows.reserve(m_items.size());
        for (int row = 0; row < m_items.size(); ++row) {
            const QString key = m_items.at(row).toMap().value(QStringLiteral("hash")).toString();
            if (!key.isEmpty()) m_hashRows.insert(key, row);
        }
        m_hashRowsValid = true;
    }
    return m_hashRows.value(hash, -1);
}

void GitItemModel::reset(QVariantList items)
{
    const qsizetype previousCount = m_items.size();
    invalidateHashRows();
    beginResetModel();
    m_items = std::move(items);
    endResetModel();
    if (previousCount != m_items.size()) {
        emit countChanged();
    }
}

bool GitItemModel::replace(QVariantList items) {
    if (m_items == items) return false;
    invalidateHashRows();
    const qsizetype previousCount = m_items.size();
    if (previousCount == 0 && items.isEmpty())
        return false;
    if (previousCount == 0) {
        beginInsertRows({}, 0, int(items.size() - 1));
        m_items = std::move(items);
        endInsertRows();
        emit countChanged();
        return true;
    }
    if (items.isEmpty()) {
        beginRemoveRows({}, 0, int(previousCount - 1));
        m_items.clear();
        endRemoveRows();
        emit countChanged();
        return true;
    }

    QStringList oldIds;
    QStringList newIds;
    QSet<QString> oldIdSet;
    QSet<QString> newIdSet;
    if (!uniqueIdentities(m_items, &oldIds, &oldIdSet) || !uniqueIdentities(items, &newIds, &newIdSet)) {
        reset(std::move(items));
        return true;
    }

    if (oldIds == newIds) {
        bool changed = false;
        int rangeStart = -1;
        for (int i = 0; i < m_items.size(); ++i) {
            if (m_items.at(i) != items.at(i)) {
                m_items[i] = items.at(i);
                changed = true;
                if (rangeStart == -1) {
                    rangeStart = i;
                }
            } else {
                if (rangeStart != -1) {
                    emit dataChanged(index(rangeStart, 0), index(i - 1, 0), {ItemRole});
                    rangeStart = -1;
                }
            }
        }
        if (rangeStart != -1) {
            emit dataChanged(index(rangeStart, 0), index(int(m_items.size() - 1), 0), {ItemRole});
        }
        return changed;
    }

    QSet<QString> intersection = oldIdSet;
    intersection.intersect(newIdSet);
    if (intersection.isEmpty()) {
        reset(std::move(items));
        return true;
    }

    QStringList oldShared;
    for (const QString &id : oldIds) {
        if (newIdSet.contains(id)) {
            oldShared.append(id);
        }
    }
    QStringList newShared;
    for (const QString &id : newIds) {
        if (oldIdSet.contains(id)) {
            newShared.append(id);
        }
    }
    if (oldShared != newShared) {
        reset(std::move(items));
        return true;
    }

    int i = 0;
    int j = 0;
    while (i < m_items.size() || j < items.size()) {
        if (i < m_items.size() && !newIdSet.contains(identityKey(m_items.at(i).toMap()))) {
            beginRemoveRows({}, i, i);
            m_items.removeAt(i);
            endRemoveRows();
        } else if (j < items.size() && !oldIdSet.contains(identityKey(items.at(j).toMap()))) {
            beginInsertRows({}, i, i);
            m_items.insert(i, items.at(j));
            endInsertRows();
            ++i;
            ++j;
        } else {
            if (m_items.at(i) != items.at(j)) {
                m_items[i] = items.at(j);
                emit dataChanged(index(i, 0), index(i, 0), {ItemRole});
            }
            ++i;
            ++j;
        }
    }

    if (m_items.size() != previousCount) {
        emit countChanged();
    }
    return true;
}

void GitItemModel::append(QVariantList items) {
    if (items.isEmpty()) return;
    const int first = m_items.size();
    if (m_hashRowsValid) {
        for (int i = 0; i < items.size(); ++i) {
            const QString key = items.at(i).toMap().value(QStringLiteral("hash")).toString();
            if (!key.isEmpty()) m_hashRows.insert(key, first + i);
        }
    }
    beginInsertRows({}, first, first + items.size() - 1);
    m_items.append(items);
    endInsertRows();
    emit countChanged();
}

void GitItemModel::prepend(QVariantList items)
{
    if (items.isEmpty()) return;
    invalidateHashRows();
    beginInsertRows({}, 0, int(items.size() - 1));
    items.append(std::move(m_items));
    m_items = std::move(items);
    endInsertRows();
    emit countChanged();
}

void GitItemModel::setItem(int row, const QVariant &item)
{
    if (row < 0 || row >= m_items.size() || m_items.at(row) == item) return;
    const QString hashKey = QStringLiteral("hash");
    if (m_items.at(row).toMap().value(hashKey) != item.toMap().value(hashKey)) invalidateHashRows();
    m_items[row] = item;
    emit dataChanged(index(row, 0), index(row, 0), {ItemRole});
}

void GitItemModel::clear() { replace({}); }

GitRepositoryService::GitRepositoryService(QObject *parent)
    : QObject(parent),
      m_git([this](const QByteArray &stderrData) {
          return m_errorMessage.isEmpty() ? cleanError(stderrData) : m_errorMessage;
      }),
      m_branches(this),
      m_worktrees(this),
      m_history(this),
      m_commitChanges(this),
      m_commitDiffFiles(this)
{
    connect(&m_git, &GitLaneScheduler::busyChanged, this, &GitRepositoryService::busyChanged);
    connect(&m_git, &GitLaneScheduler::taskStarted, this, &GitRepositoryService::taskStarted);
    connect(&m_git, &GitLaneScheduler::taskFinished, this, &GitRepositoryService::taskFinished);
    connect(&m_git, &GitLaneScheduler::errorReported, this, &GitRepositoryService::setError);
    connect(&m_git, &GitLaneScheduler::requestSuperseded, this, [this](GitLane lane) {
        // The superseded history load carried the branch follow-on with it.
        if (lane == GitLane::History)
            m_pendingIncludeBranches = true;
    });
    connect(&m_git, &GitLaneScheduler::staleLaneFinished, this, [this](GitLane lane) {
        // A switch that deferred discovery past running lanes owes the new
        // path a discovery pass once the last lane is idle; a stale Status
        // lane always re-discovers because it just vacated the lane that
        // discovery itself uses.
        if (lane == GitLane::Status || (m_pendingDiscover && !m_git.anyLaneRunning())) {
            m_pendingDiscover = false;
            discoverProject();
        }
    });
    connect(&m_repositoryAppearWatcher, &QFileSystemWatcher::directoryChanged, this, [this](const QString &) {
        // Only a .git appearing is worth a discovery pass; ordinary files
        // coming and going in the project root are not.
        if (m_available || m_projectPath.isEmpty()
            || !QFileInfo::exists(QDir(m_projectPath).filePath(QStringLiteral(".git"))))
            return;
        m_repositoryAppearWatcher.removePaths(m_repositoryAppearWatcher.directories());
        // In the background, showing the project again discovers it.
        discoverProject();
    });
    connect(&m_watcher, &QFileSystemWatcher::fileChanged, this, [this](const QString &path) {
        configureWatcher();
        scheduleRefresh(watcherImpliesBranchRefresh(path));
    });
    connect(&m_watcher, &QFileSystemWatcher::directoryChanged, this, [this](const QString &path) {
        configureWatcher();
        scheduleRefresh(watcherImpliesBranchRefresh(path));
    });
    connect(&m_workTreeWatcher, &span::platform::DirectoryChangeWatcher::changed, this,
            &GitRepositoryService::noteWorkTreeChanges);
    m_pollTimer.setInterval(pollIntervalMs());
    connect(&m_pollTimer, &QTimer::timeout, this, [this]() {
        // While discovery keeps failing (slow mount, dubious ownership fixed
        // on disk, transient lock) the poll doubles as the retry loop.
        if (!m_available && !m_projectPath.isEmpty()) {
            discoverProject();
            return;
        }
        // Index, HEAD and ref changes refresh through m_watcher; a poll only
        // picks up working-tree edits, so a quiet watched tree starts no git.
        if (!m_workTreeDirty && m_workTreeWatcher.watching()) return;
        scheduleRefresh(false);
    });
    m_refreshCoalesce.setSingleShot(true);
    m_refreshCoalesce.setInterval(kRefreshCoalesceMs);
    m_refreshCoalesce.setTimerType(Qt::CoarseTimer);
    connect(&m_refreshCoalesce, &QTimer::timeout,
            this, &GitRepositoryService::flushPendingRefresh);
}

GitRepositoryService::~GitRepositoryService() {
    m_pollTimer.stop();
    m_refreshCoalesce.stop();
    disconnect(&m_watcher, nullptr, this, nullptr);
    disconnect(&m_repositoryAppearWatcher, nullptr, this, nullptr);
    disconnect(&m_workTreeWatcher, nullptr, this, nullptr);
    // m_git kills its processes without running their completions.
    disconnect(&m_git, nullptr, this, nullptr);
}

bool GitRepositoryService::available() const { return m_available; }
QString GitRepositoryService::projectPath() const { return m_projectPath; }
QString GitRepositoryService::repositoryRoot() const { return m_repositoryRoot; }
QString GitRepositoryService::currentBranch() const { return m_currentBranch; }
bool GitRepositoryService::busy() const { return m_git.busy(); }

bool GitRepositoryService::historyViewLoading() const
{
    // Overlay only while this surface has nothing to show. Remaining history
    // chunks and follow-on commit loads must not keep the log unusable.
    return m_history.count() == 0 && m_git.laneBusy(GitLane::History);
}

bool GitRepositoryService::commitViewLoading() const
{
    return m_git.laneBusy(GitLane::Diff);
}

bool GitRepositoryService::changesViewLoading() const
{
    const QString operation = m_git.laneOperation(GitLane::Status);
    return m_git.laneBusy(GitLane::Status)
        && (operation == QLatin1String("refresh") || operation == QLatin1String("discover"));
}

QString GitRepositoryService::errorMessage() const { return m_errorMessage; }
QString GitRepositoryService::selectedCommit() const { return m_selectedCommit; }
QVariantMap GitRepositoryService::selectedCommitDetails() const { return m_selectedCommitDetails; }
GitItemModel *GitRepositoryService::branches() { return &m_branches; }
GitItemModel *GitRepositoryService::worktrees()
{
    return &m_worktrees;
}
bool GitRepositoryService::refsRefreshing() const
{
    return m_refsRefreshing;
}
QVariantMap GitRepositoryService::worktreeChanges() const
{
    return m_worktreeChanges;
}
GitItemModel *GitRepositoryService::history() { return &m_history; }
bool GitRepositoryService::historyHasMore() const
{
    return m_historyHasMore;
}
GitItemModel *GitRepositoryService::commitChanges() { return &m_commitChanges; }
GitItemModel *GitRepositoryService::commitDiffFiles() { return &m_commitDiffFiles; }
int GitRepositoryService::diffAdditions() const { return m_diffAdditions; }
int GitRepositoryService::diffDeletions() const { return m_diffDeletions; }
int GitRepositoryService::pollIntervalMs() const
{
    return m_idle ? kIdlePollMs : kActivePollMs;
}

void GitRepositoryService::setIdle(bool idle) {
    if (m_idle == idle) {
        return;
    }
    const bool wasIdle = m_idle;
    m_idle = idle;
    m_pollTimer.setInterval(pollIntervalMs());
    if (m_pollTimer.isActive()) {
        // QTimer keeps the old interval until restarted.
        m_pollTimer.start();
    }
    if (wasIdle) {
        scheduleRefresh(false);
    }
}

bool GitRepositoryService::foreground() const
{
    return m_foreground;
}
bool GitRepositoryService::polling() const
{
    return m_pollTimer.isActive();
}

void GitRepositoryService::setForeground(bool foreground)
{
    if (m_foreground == foreground) {
        return;
    }
    m_foreground = foreground;
    if (!foreground) {
        // Running reads finish; nothing new starts until the project shows
        // again, and that refresh covers whatever changed meanwhile.
        m_pollTimer.stop();
        m_refreshCoalesce.stop();
        m_workTreeWatcher.stop();
        return;
    }
    if (m_projectPath.isEmpty()) {
        return;
    }
    if (!m_available) {
        // A pending discovery runs once the unwinding lanes are idle; an
        // ordinary directory stays with its watcher until a .git appears.
        if (!m_pendingDiscover && m_repositoryAppearWatcher.directories().isEmpty()) discoverProject();
        return;
    }
    m_refreshCoalesce.stop();
    watchWorkTree();
    refreshStatus(true, false);
    startPolling();
}

void GitRepositoryService::startPolling()
{
    if (m_foreground) m_pollTimer.start();
}

void GitRepositoryService::setPollIntervalForTesting(int milliseconds)
{
    m_pollTimer.setInterval(std::max(1, milliseconds));
    if (m_pollTimer.isActive()) m_pollTimer.start();
}

void GitRepositoryService::watchWorkTree()
{
    // The refresh that follows clears the flag; an unwatchable tree keeps
    // the plain poll.
    m_workTreeDirty = true;
    m_workTreeWatcher.watch(m_repositoryRoot);
}

void GitRepositoryService::noteWorkTreeChanges(const QStringList &relativePaths)
{
    if (relativePaths.isEmpty()) {
        m_workTreeDirty = true;
        return;
    }
    // Changes inside the git directory are m_watcher's (and our own reads').
    const auto insideGitDirectory = [this](const QString &absolute) {
        for (const QString &gitDir : {m_gitDirectory, m_gitCommonDirectory}) {
            if (gitDir.isEmpty()) continue;
            if (absolute.compare(gitDir, Qt::CaseInsensitive) == 0
                || absolute.startsWith(gitDir + QLatin1Char('/'), Qt::CaseInsensitive))
                return true;
        }
        return false;
    };
    const QDir root(m_repositoryRoot);
    for (const QString &path : relativePaths) {
        if (!insideGitDirectory(QDir::cleanPath(root.filePath(path)))) {
            m_workTreeDirty = true;
            return;
        }
    }
}

void GitRepositoryService::setDiffStats(int additions, int deletions,
                                        const QHash<QString, QPair<int, int>> &perFileStats) {
    if (m_diffAdditions != additions || m_diffDeletions != deletions) {
        m_diffAdditions = additions;
        m_diffDeletions = deletions;
        emit diffStatsChanged();
    }
    if (perFileStats.isEmpty())
        return;
    QVariantList items = m_commitChanges.items();
    bool changed = false;
    for (QVariant &itemVal : items) {
        QVariantMap map = itemVal.toMap();
        const QString path = map.value(QStringLiteral("path")).toString();
        if (perFileStats.contains(path)) {
            const auto pair = perFileStats.value(path);
            if (map.value(QStringLiteral("additions")).toInt() != pair.first
                || map.value(QStringLiteral("deletions")).toInt() != pair.second) {
                map.insert(QStringLiteral("additions"), pair.first);
                map.insert(QStringLiteral("deletions"), pair.second);
                itemVal = map;
                changed = true;
            }
        }
    }
    if (changed)
        m_commitChanges.replace(std::move(items));
}

void GitRepositoryService::rebuildCommitChanges(const QVariantList &staged,
                                                const QVariantList &working) {
    QHash<QString, QPair<int, int>> previousStats;
    for (const QVariant &itemVal : m_commitChanges.items()) {
        const QVariantMap map = itemVal.toMap();
        const QString path = map.value(QStringLiteral("path")).toString();
        if (path.isEmpty())
            continue;
        previousStats.insert(path, qMakePair(map.value(QStringLiteral("additions")).toInt(),
                                             map.value(QStringLiteral("deletions")).toInt()));
    }
    // Rows keep their last counted stats until the next numstat lands.
    const auto restoreStats = [&previousStats](QVariantMap &item, const QString &path) {
        if (!previousStats.contains(path))
            return;
        const auto pair = previousStats.value(path);
        item.insert(QStringLiteral("additions"), pair.first);
        item.insert(QStringLiteral("deletions"), pair.second);
    };

    QMap<QString, QVariantMap> byPath;
    for (const QVariant &value : working) {
        QVariantMap item = value.toMap();
        const QString path = item.value(QStringLiteral("path")).toString();
        restoreStats(item, path);
        byPath.insert(path, item);
    }
    for (const QVariant &value : staged) {
        QVariantMap stagedItem = value.toMap();
        const QString path = stagedItem.value(QStringLiteral("path")).toString();
        if (byPath.contains(path)) {
            QVariantMap item = byPath.value(path);
            if (statusPriority(stagedItem.value(QStringLiteral("status")).toString())
                > statusPriority(item.value(QStringLiteral("status")).toString())) {
                item.insert(QStringLiteral("status"), stagedItem.value(QStringLiteral("status")));
            }
            byPath.insert(path, item);
        } else {
            restoreStats(stagedItem, path);
            byPath.insert(path, stagedItem);
        }
    }

    QVariantList combined;
    combined.reserve(byPath.size());
    for (auto it = byPath.cbegin(); it != byPath.cend(); ++it)
        combined.push_back(it.value());
    m_commitChanges.replace(std::move(combined));
}



void GitRepositoryService::setProjectPath(const QString &path) {
    const QString normalized = path.trimmed().isEmpty()
        ? QString() : QDir::cleanPath(QFileInfo(path).absoluteFilePath());
    if (m_projectPath == normalized && (m_available || normalized.isEmpty())) return;

    m_git.invalidateResults();
    m_projectPath = normalized;
    emit projectPathChanged();
    clearRepository();
    if (m_git.cancelAll() || m_git.busy()) {
        // Lanes are still unwinding; they owe this path a discovery pass.
        m_pendingDiscover = true;
        return;
    }
    m_pendingDiscover = false;
    discoverProject();
}

void GitRepositoryService::discoverProject() {
    // A background project discovers when it shows again (setForeground).
    if (m_projectPath.isEmpty() || !m_foreground) return;
    runGitAt(m_projectPath,
             {QStringLiteral("rev-parse"), QStringLiteral("--show-toplevel"), QStringLiteral("--absolute-git-dir"),
              QStringLiteral("--git-common-dir")},
             kGitOperationDiscover, [this](bool ok, const QByteArray &out, const QByteArray &err) {
                 if (!ok) {
                     // Other errors (dubious ownership, corruption, a dead git) must
                     // surface and retry on the poll; a plain directory waits for .git.
                     const QString message = cleanError(err);
                     if (message.contains(QLatin1String("not a git repository"), Qt::CaseInsensitive)) {
                         setError(QString());
                         watchForRepository();
                     } else {
                         setError(message);
                         startPolling();
                     }
                     return;
                 }
                 const QStringList paths = QString::fromUtf8(out).split('\n', Qt::SkipEmptyParts);
                 if (paths.isEmpty()) return;
                 m_repositoryRoot = QDir::cleanPath(paths.at(0).trimmed());
                 m_gitDirectory = paths.size() > 1 ? QDir::cleanPath(paths.at(1).trimmed())
                                                   : QDir(m_repositoryRoot).filePath(QStringLiteral(".git"));
                 m_gitCommonDirectory = paths.size() > 2 ? paths.at(2).trimmed() : m_gitDirectory;
                 if (QDir::isRelativePath(m_gitCommonDirectory))
                     m_gitCommonDirectory = QDir::cleanPath(QDir(m_projectPath).absoluteFilePath(m_gitCommonDirectory));
                 else
                     m_gitCommonDirectory = QDir::cleanPath(m_gitCommonDirectory);
                 m_available = !m_repositoryRoot.isEmpty();
                 if (!m_repositoryAppearWatcher.directories().isEmpty())
                     m_repositoryAppearWatcher.removePaths(m_repositoryAppearWatcher.directories());
                 emit repositoryChanged();
                 configureWatcher();
                 watchWorkTree();
                 startPolling();
                 refreshStatus(true);
             });
}

void GitRepositoryService::watchForRepository()
{
    m_pollTimer.stop();
    QStringList paths{m_projectPath};
    // `git init` creates .git before filling it: a discovery that raced it
    // waits for the rest to land inside.
    const QString dotGit = QDir(m_projectPath).filePath(QStringLiteral(".git"));
    if (QFileInfo(dotGit).isDir()) paths.append(dotGit);
    for (const QString &path : paths) {
        if (!m_repositoryAppearWatcher.directories().contains(path)) m_repositoryAppearWatcher.addPath(path);
    }
}

void GitRepositoryService::refresh() {
    if (m_available) refreshStatus(true);
    else if (!m_available && !m_projectPath.isEmpty()) setProjectPath(m_projectPath);
}


void GitRepositoryService::clearError() {
    setError(QString());
}




QVariantMap GitRepositoryService::statusMap() const {
    QVariantMap result;
    for (auto it = m_statuses.cbegin(); it != m_statuses.cend(); ++it)
        result.insert(it.key(), it.value());
    return result;
}

void GitRepositoryService::requestBaseContent(const QString &absolutePath) {
    QString relative;
    if (!resolveWorktreePath(absolutePath, nullptr, &relative)) {
        emit baseContentReady(absolutePath, QString(), false);
        return;
    }
    runGit({QStringLiteral("show"), QStringLiteral("HEAD:") + relative}, QStringLiteral("base-content"),
           [this, absolutePath](bool ok, const QByteArray &out, const QByteArray &) {
        emit baseContentReady(absolutePath, ok ? QString::fromUtf8(out) : QString(), ok);
    }, false);
}

bool GitRepositoryService::isSafeGitObjectId(const QString &hash)
{
    // 40 hex digits for SHA-1 repositories, 64 for SHA-256 ones.
    static const QRegularExpression re(QStringLiteral("^[0-9a-fA-F]{7,64}$"));
    return re.match(hash).hasMatch();
}

bool GitRepositoryService::isSafeGitRef(const QString &ref)
{
    if (ref.isEmpty() || ref.startsWith(QLatin1Char('-'))) {
        return false;
    }
    if (ref.contains(QLatin1Char('\0')) || ref.contains(QLatin1Char('\n'))
        || ref.contains(QLatin1Char('\r')) || ref.contains(QLatin1String(".."))) {
        return false;
    }
    if (ref.contains(QLatin1Char(' ')) || ref.contains(QLatin1Char('\t'))) {
        return false;
    }
    return true;
}

bool GitRepositoryService::resolveWorktreePath(const QString &path, QString *absoluteOut,
                                               QString *relativeOut) const
{
    if (m_repositoryRoot.isEmpty() || path.trimmed().isEmpty())
        return false;
    const QString cleaned = QDir::cleanPath(path.trimmed());
    if (cleaned == QLatin1String("..") || cleaned.startsWith(QLatin1String("../"))
        || cleaned.startsWith(QLatin1String("..\\"))) {
        return false;
    }
    QString absolute;
    if (QFileInfo(cleaned).isAbsolute()) {
        absolute = QDir::cleanPath(cleaned);
    } else {
        absolute = QDir::cleanPath(QDir(m_repositoryRoot).filePath(cleaned));
    }
    const QString root = QDir::cleanPath(m_repositoryRoot);
    const Qt::CaseSensitivity sensitivity = Qt::CaseInsensitive;
    if (QString::compare(absolute, root, sensitivity) == 0)
        return false;
    const QString rootPrefix = root.endsWith(QLatin1Char('/')) ? root : root + QLatin1Char('/');
    if (!absolute.startsWith(rootPrefix, sensitivity))
        return false;
    if (absoluteOut)
        *absoluteOut = absolute;
    if (relativeOut)
        *relativeOut = QDir(root).relativeFilePath(absolute);
    return true;
}
