#include "workspace/file_tree_model.h"

#include "core/product_paths.h"
#include "platform/windows/process_containment.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDesktopServices>
#include <QDir>
#include <QFile>
#include <QFutureWatcher>
#include <QRegularExpression>
#include <QUrl>
#include <QUuid>
#include <QtConcurrent>
#include <QFileSystemWatcher>
#include <QTimer>

#include <algorithm>

namespace {
constexpr int kWatchCoalesceMs = 250;
constexpr int kMountedChildLimit = 32;
constexpr int kMaxWatchedDirectories = 60;

bool isLink(const QFileInfo &info)
{
    return info.isSymbolicLink() || info.isJunction();
}

// Containment compares resolved paths, so symlinks and junctions that leave
// the project count as outside; paths that do not resolve compare cleaned.
// Resolving costs a handle open per call, so only links are resolved: any
// other path stays where its lexical location says.
QString containmentKey(const QString &path) {
    const QString canonical = QFileInfo(path).canonicalFilePath();
    return canonical.isEmpty() ? QDir::cleanPath(path) : canonical;
}

QString containmentKey(const QFileInfo &info)
{
    return isLink(info) ? containmentKey(info.absoluteFilePath()) : QDir::cleanPath(info.absoluteFilePath());
}

bool isSameOrInside(const QString &rootKey, const QString &candidate) {
    if (rootKey.isEmpty() || candidate.isEmpty()) {
        return false;
    }
    if (QString::compare(candidate, rootKey, Qt::CaseInsensitive) == 0) {
        return true;
    }
    const QString prefix = rootKey.endsWith(QLatin1Char('/')) ? rootKey : rootKey + QLatin1Char('/');
    return candidate.startsWith(prefix, Qt::CaseInsensitive);
}
}

FileTreeModel::FileTreeModel(QObject *parent)
    : QAbstractListModel(parent)
{
    m_watchCoalesce.setSingleShot(true);
    m_watchCoalesce.setInterval(kWatchCoalesceMs);
    m_watchCoalesce.setTimerType(Qt::CoarseTimer);
    connect(&m_watchCoalesce, &QTimer::timeout, this, &FileTreeModel::flushWatchedRefresh);
    connect(&m_watcher, &QFileSystemWatcher::directoryChanged,
            this, &FileTreeModel::onWatchedPathChanged);
    connect(&m_watcher, &QFileSystemWatcher::fileChanged,
            this, &FileTreeModel::onWatchedPathChanged);
}

FileTreeModel::~FileTreeModel()
{
    m_watchCoalesce.stop();
    ++m_watchGeneration;
    ++m_loadGeneration;
}

int FileTreeModel::mountedChildLimit()
{
    return kMountedChildLimit;
}

int FileTreeModel::rowCount(const QModelIndex &parent) const {
    return parent.isValid() ? 0 : m_nodes.size();
}

QVariant FileTreeModel::data(const QModelIndex &index, int role) const {
    if (!index.isValid() || index.row() < 0 || index.row() >= m_nodes.size()) {
        return {};
    }

    const auto &node = m_nodes.at(index.row());
    switch (role) {
    case NameRole:
        return node.name;
    case PathRole:
        return node.path;
    case DirectoryRole:
        return node.isDir;
    case DepthRole:
        return node.depth;
    case ExpandedRole:
        return node.expanded;
    case RootRole:
        return node.isRoot;
    case GitStatusRole:
        return directGitStatus(node.path);
    case AggregateGitStatusRole:
        return node.isDir ? aggregateGitStatus(node.path) : directGitStatus(node.path);
    case CategoryRole:
        // Relative to the root: folders above the project (D:\tests\app) say
        // nothing about the files in it.
        return categoryForPath(QDir(m_rootPath).relativeFilePath(node.path), node.isDir);
    case LoadingRole:
        return node.loading;
    case RevealedRole:
        return node.revealed;
    default:
        return {};
    }
}

QHash<int, QByteArray> FileTreeModel::roleNames() const {
    return {
        {NameRole, "name"},
        {PathRole, "path"},
        {DirectoryRole, "isDir"},
        {DepthRole, "depth"},
        {ExpandedRole, "expanded"},
        {RootRole, "isRoot"},
        {GitStatusRole, "gitStatus"},
        {AggregateGitStatusRole, "aggregateGitStatus"},
        {CategoryRole, "category"},
        {LoadingRole, "loading"},
        {RevealedRole, "revealed"},
    };
}

QString FileTreeModel::rootPath() const {
    return m_rootPath;
}

QString FileTreeModel::errorMessage() const {
    return m_errorMessage;
}

bool FileTreeModel::canUndo() const {
    return !m_deleteStack.isEmpty();
}

bool FileTreeModel::busy() const {
    return m_busy;
}

int FileTreeModel::nodeCount() const { return m_nodes.size(); }

void FileTreeModel::setTrashRootForTesting(const QString &path) {
    m_trashRootForTesting = path;
}

void FileTreeModel::setCopyMovesForTesting(bool enabled) {
    m_copyMovesForTesting = enabled;
}

void FileTreeModel::setWatchCoalesceIntervalForTesting(int milliseconds) {
    m_watchCoalesce.setInterval(milliseconds);
}

void FileTreeModel::clearError() {
    setError(QString());
}

void FileTreeModel::openPathExternally(const QString &path) {
    const QString target = path.isEmpty() ? m_rootPath : path;
    if (!target.isEmpty() && pathIsInsideRoot(target)) {
        QDesktopServices::openUrl(QUrl::fromLocalFile(target));
    }
}

int FileTreeModel::rowForPath(const QString &path) const {
    if (path.isEmpty()) return -1;
    const QString clean = QDir::cleanPath(path);
    for (int i = 0; i < m_nodes.size(); ++i) {
        if (QDir::cleanPath(m_nodes.at(i).path) == clean) {
            return i;
        }
    }
    return -1;
}

bool FileTreeModel::canRunInProjectTerminal(int row) const {
    if (row < 0 || row >= m_nodes.size()) {
        return false;
    }

    const Node &node = m_nodes.at(row);
    const QFileInfo info(node.path);
    if (node.isDir || !info.isFile() || !pathIsInsideRoot(node.path)) {
        return false;
    }

    const QString suffix = info.suffix().toLower();
    return suffix == QStringLiteral("bat")
        || suffix == QStringLiteral("cmd")
        || suffix == QStringLiteral("ps1");
}

QString FileTreeModel::projectTerminalCommand(int row) const {
    if (!canRunInProjectTerminal(row)) {
        return {};
    }

    const QFileInfo info(m_nodes.at(row).path);
    QString path = info.canonicalFilePath();
    if (path.isEmpty()) {
        path = info.absoluteFilePath();
    }
    path = QDir::toNativeSeparators(path);
    if (path.contains(QLatin1Char('"'))
        || path.contains(QLatin1Char('\r'))
        || path.contains(QLatin1Char('\n'))) {
        return {};
    }

    // A single-quoted PowerShell literal keeps spaces and shell metacharacters
    // in the file name inert. Doubling apostrophes is PowerShell's literal
    // escape, and -NoExit leaves the project terminal open for inspection.
    path.replace(QLatin1Char('\''), QStringLiteral("''"));
    return QStringLiteral(
               "powershell.exe -NoLogo -NoProfile -ExecutionPolicy Bypass "
               "-NoExit -Command \"& '%1'\"")
        .arg(path);
}

QStringList FileTreeModel::independentTerminalArguments(int row) const {
    const QString commandLine = projectTerminalCommand(row);
    if (commandLine.isEmpty() || m_rootPath.isEmpty()) {
        return {};
    }
    QStringList arguments{
        QStringLiteral("--terminal"),
        QStringLiteral("--cwd"),
        m_rootPath,
        QStringLiteral("--command"),
        commandLine,
    };
    // A window opened from a --data-dir run keeps its logs in the same root.
    const QString dataDir = span::core::ProductPaths::rootOverride();
    if (!dataDir.isEmpty()) {
        arguments << QStringLiteral("--data-dir") << QDir::toNativeSeparators(dataDir);
    }
    return arguments;
}

bool FileTreeModel::runInIndependentTerminal(int row) {
    const QStringList arguments = independentTerminalArguments(row);
    if (arguments.isEmpty()) {
        return false;
    }
    QString launchError;
    const quint32 processId = span::platform::process::startOwnedProcess(
        QCoreApplication::applicationFilePath(), arguments, m_rootPath, &launchError);
    if (processId == 0) {
        setError(launchError.isEmpty()
                     ? tr("Failed to start the independent terminal window.")
                     : tr("Failed to start the independent terminal window: %1")
                           .arg(launchError));
        return false;
    }
    return true;
}

void FileTreeModel::setGitStatusMap(const QVariantMap &statuses, const QString &repositoryRoot) {
    QHash<QString, QString> next;
    for (auto it = statuses.cbegin(); it != statuses.cend(); ++it)
        next.insert(QDir::fromNativeSeparators(it.key()), it.value().toString());
    const QString cleanRoot = QDir::cleanPath(repositoryRoot);
    if (next == m_gitStatuses && cleanRoot == m_repositoryRoot) return;
    m_gitStatuses = std::move(next);
    m_repositoryRoot = cleanRoot;
    rebuildAggregateGitStatuses();
    if (!m_nodes.isEmpty())
        emit dataChanged(index(0, 0), index(m_nodes.size() - 1, 0),
                         {GitStatusRole, AggregateGitStatusRole});
}

QString FileTreeModel::relativeRepositoryPath(const QString &path) const {
    if (m_repositoryRoot.isEmpty()) return {};
    return QDir::fromNativeSeparators(QDir(m_repositoryRoot).relativeFilePath(path));
}

QString FileTreeModel::directGitStatus(const QString &path) const {
    QString relative = relativeRepositoryPath(path);
    if (relative == QLatin1String("."))
        relative.clear();
    if (relative.isEmpty())
        return {};
    const QString exact = m_gitStatuses.value(relative);
    if (!exact.isEmpty())
        return exact;
    QString prefix = relative;
    while (true) {
        const int slash = prefix.lastIndexOf(QLatin1Char('/'));
        if (slash < 0)
            break;
        prefix = prefix.left(slash);
        if (m_gitStatuses.value(prefix) == QLatin1String("ignored"))
            return QStringLiteral("ignored");
    }
    return {};
}

int FileTreeModel::gitStatusPriority(const QString &status) {
    if (status == QStringLiteral("conflict")) return 6;
    if (status == QStringLiteral("deleted")) return 5;
    if (status == QStringLiteral("modified") || status == QStringLiteral("type-changed")) return 4;
    if (status == QStringLiteral("renamed") || status == QStringLiteral("copied")) return 3;
    if (status == QStringLiteral("added")) return 2;
    return 0;
}

void FileTreeModel::rebuildAggregateGitStatuses() {
    m_aggregateStatuses.clear();
    for (auto it = m_gitStatuses.cbegin(); it != m_gitStatuses.cend(); ++it) {
        const int priority = gitStatusPriority(it.value());
        if (priority <= 0)
            continue;
        QString prefix = it.key();
        while (true) {
            auto existing = m_aggregateStatuses.find(prefix);
            if (existing == m_aggregateStatuses.end()
                || gitStatusPriority(existing.value()) < priority)
                m_aggregateStatuses.insert(prefix, it.value());
            const int slash = prefix.lastIndexOf(QLatin1Char('/'));
            if (slash < 0)
                break;
            prefix = prefix.left(slash);
        }
        auto root = m_aggregateStatuses.find(QString());
        if (root == m_aggregateStatuses.end()
            || gitStatusPriority(root.value()) < priority)
            m_aggregateStatuses.insert(QString(), it.value());
    }
}

QString FileTreeModel::aggregateGitStatus(const QString &path) const {
    QString relative = relativeRepositoryPath(path);
    if (relative == QStringLiteral("."))
        relative.clear();
    const QString aggregate = m_aggregateStatuses.value(relative);
    if (!aggregate.isEmpty())
        return aggregate;
    return directGitStatus(path);
}

QString FileTreeModel::categoryForPath(const QString &path, bool directory) {
    const QString normalized = QDir::fromNativeSeparators(path).toLower();
    const QString name = QFileInfo(path).fileName().toLower();
    const QStringList segments = normalized.split('/', Qt::SkipEmptyParts);
    const auto has = [&segments](const QStringList &values) {
        for (const QString &segment : segments) if (values.contains(segment)) return true;
        return false;
    };
    if (name.startsWith(QStringLiteral("build")) || name == QStringLiteral("dist")
        || name == QStringLiteral("out") || name == QStringLiteral("target")
        || has({QStringLiteral("generated"), QStringLiteral(".cache")})) return QStringLiteral("build");
    if (has({QStringLiteral("test"), QStringLiteral("tests"), QStringLiteral("spec"),
             QStringLiteral("specs"), QStringLiteral("__tests__")})) return QStringLiteral("test");
    if (has({QStringLiteral("node_modules"), QStringLiteral("vendor"), QStringLiteral("third_party"),
             QStringLiteral("venv"), QStringLiteral(".venv")})) return QStringLiteral("dependency");
    if (has({QStringLiteral("docs"), QStringLiteral("doc")})) return QStringLiteral("docs");
    if (has({QStringLiteral("assets"), QStringLiteral("resources"), QStringLiteral("public"),
             QStringLiteral("static")})) return QStringLiteral("assets");
    if (has({QStringLiteral(".github"), QStringLiteral(".vscode"), QStringLiteral(".codex"),
             QStringLiteral("config")})) return QStringLiteral("config");
    if (!directory) {
        const QString suffix = QFileInfo(path).suffix().toLower();
        if (QStringList{QStringLiteral("cmake"), QStringLiteral("ninja"), QStringLiteral("obj"),
                        QStringLiteral("o"), QStringLiteral("dll"), QStringLiteral("exe")}.contains(suffix))
            return QStringLiteral("build");
        if (name.contains(QStringLiteral("test")) || name.contains(QStringLiteral("spec")))
            return QStringLiteral("test");
    }
    return {};
}

void FileTreeModel::setRootPath(const QString &path) {
    if (path.trimmed().isEmpty()) {
        clearRoot();
        return;
    }
    const QFileInfo info(path);
    if (!info.exists() || !info.isDir()) {
        setError(tr("Root path is not a directory: %1").arg(path));
        return;
    }

    const QString canonical = info.canonicalFilePath();
    if (m_rootPath == canonical) {
        return;
    }

    const quint64 generation = ++m_loadGeneration;
    ++m_watchGeneration;
    m_watchCoalesce.stop();
    beginAsyncLoad();
    beginResetModel();
    m_rootPath = canonical;
    m_nodes.clear();
    m_nodes.push_back({info.fileName().isEmpty() ? canonical : info.fileName(),
                       canonical, true, true, false, 0, true});
    endResetModel();
    setError(QString());
    auto *watcher = new QFutureWatcher<QVector<Node>>(this);
    connect(watcher, &QFutureWatcher<QVector<Node>>::finished, this,
            [this, watcher, generation, canonical]() {
        const QVector<Node> children = watcher->result();
        watcher->deleteLater();
        if (generation == m_loadGeneration && m_rootPath == canonical
            && !m_nodes.isEmpty()) {
            if (!children.isEmpty()) {
                beginInsertRows({}, 1, children.size());
                for (const Node &child : children)
                    m_nodes.push_back(child);
                endInsertRows();
            }
            m_nodes[0].loaded = true;
            m_nodes[0].keepMounted = children.size() <= kMountedChildLimit;
            m_nodes[0].revealed = true;
            syncWatchedDirectories();
        }
        endAsyncLoad();
    });
    watcher->setFuture(QtConcurrent::run([canonical]() {
        return readChildrenForRoot(canonical, 1, canonical);
    }));
}

void FileTreeModel::clearRoot() {
    if (m_rootPath.isEmpty() && m_nodes.isEmpty()) {
        return;
    }
    m_watchCoalesce.stop();
    ++m_watchGeneration;
    ++m_loadGeneration;
    if (!m_watcher.directories().isEmpty())
        m_watcher.removePaths(m_watcher.directories());
    if (!m_watcher.files().isEmpty())
        m_watcher.removePaths(m_watcher.files());
    beginResetModel();
    m_rootPath.clear();
    m_nodes.clear();
    endResetModel();
}

void FileTreeModel::refresh() {
    refreshPreservingExpansion(true);
}

void FileTreeModel::refreshPreservingExpansion(bool reportBusy) {
    if (m_rootPath.isEmpty()) {
        return;
    }
    QSet<QString> expanded;
    for (const Node &node : m_nodes) {
        if (node.isDir && node.expanded)
            expanded.insert(node.path);
    }
    const QSet<QString> mounted = mountedDirectoryPaths();
    const QString rootPath = m_rootPath;
    const quint64 watchGeneration = m_watchGeneration;
    const quint64 loadGeneration = reportBusy ? ++m_loadGeneration : m_loadGeneration;
    if (reportBusy)
        beginAsyncLoad();
    else
        m_silentRebuild = true;
    auto *watcher = new QFutureWatcher<QVector<Node>>(this);
    connect(watcher, &QFutureWatcher<QVector<Node>>::finished, this,
            [this, watcher, rootPath, watchGeneration, loadGeneration, reportBusy]() {
        const QVector<Node> nodes = watcher->result();
        watcher->deleteLater();
        const bool stale = m_rootPath != rootPath
                           || m_watchGeneration != watchGeneration
                           || (reportBusy && loadGeneration != m_loadGeneration);
        if (!stale)
            applyRebuiltTree(nodes);
        if (reportBusy)
            endAsyncLoad();
        else
            m_silentRebuild = false;
    });
    watcher->setFuture(QtConcurrent::run([rootPath, expanded, mounted]() {
        return buildMountedTree(rootPath, expanded, mounted);
    }));
}

void FileTreeModel::toggleExpanded(int row) {
    if (row < 0 || row >= m_nodes.size() || !m_nodes.at(row).isDir) {
        return;
    }

    if (m_nodes.at(row).expanded) {
        // Flip the chevron immediately. Small loaded directories keep their
        // children mounted so QML can animate height; large ones drop rows.
        m_nodes[row].expanded = false;
        m_nodes[row].loading = false;
        emit dataChanged(index(row), index(row), {ExpandedRole, LoadingRole});
        if (m_nodes.at(row).keepMounted && m_nodes.at(row).loaded) {
            updateDescendantReveal(row);
            syncWatchedDirectories();
            return;
        }
        collapseRemovingDescendants(row);
        syncWatchedDirectories();
        return;
    }

    // Expand flag first so the UI responds even if directory IO is slow.
    m_nodes[row].expanded = true;
    emit dataChanged(index(row), index(row), {ExpandedRole});
    if (m_nodes.at(row).keepMounted && m_nodes.at(row).loaded) {
        m_nodes[row].loading = false;
        emit dataChanged(index(row), index(row), {LoadingRole});
        updateDescendantReveal(row);
        syncWatchedDirectories();
        return;
    }
    m_nodes[row].loading = true;
    emit dataChanged(index(row), index(row), {LoadingRole});
    loadChildren(row);
}

QString FileTreeModel::newChildPath(const QString &parentPath, const QString &name) {
    if (!validateName(name)) {
        return {};
    }
    // An empty parent means the project root; a file parent creates a sibling.
    QString parent = parentPath.isEmpty() ? m_rootPath : parentPath;
    if (QFileInfo(parent).isFile()) {
        parent = QFileInfo(parent).dir().absolutePath();
    }
    if (parent.isEmpty() || !pathIsInsideRoot(parent)) {
        setError(tr("Parent path is outside the project root."));
        return {};
    }
    const QString path = QDir(parent).filePath(name.trimmed());
    if (!pathIsInsideRoot(path)) {
        setError(tr("Path is outside the project root."));
        return {};
    }
    return path;
}

void FileTreeModel::createFile(const QString &parentPath, const QString &name) {
    const QString path = newChildPath(parentPath, name);
    if (path.isEmpty()) {
        return;
    }
    if (QFileInfo::exists(path)) {
        setError(tr("Path already exists: %1").arg(path));
        return;
    }
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::NewOnly)) {
        setError(tr("Could not create file: %1").arg(path));
        return;
    }
    file.close();
    refresh();
    setError(QString());
    emit operationCompleted(tr("Created file %1").arg(name.trimmed()));
}

void FileTreeModel::createFolder(const QString &parentPath, const QString &name) {
    const QString path = newChildPath(parentPath, name);
    if (path.isEmpty()) {
        return;
    }
    if (!QDir().mkpath(path)) {
        setError(tr("Could not create folder: %1").arg(path));
        return;
    }
    refresh();
    setError(QString());
    emit operationCompleted(tr("Created folder %1").arg(name.trimmed()));
}

void FileTreeModel::renamePath(const QString &from, const QString &name) {
    if (from.isEmpty() || !pathIsInsideRoot(from) || !validateName(name)) {
        return;
    }
    const QString cleanFrom = QDir::cleanPath(from);
    const QString cleanRoot = QDir::cleanPath(m_rootPath);
    if (cleanFrom == cleanRoot) {
        return;
    }
    const QString to = QFileInfo(from).dir().filePath(name.trimmed());
    if (!pathIsInsideRoot(to)) {
        setError(tr("Destination is outside the project root."));
        return;
    }
    const bool sameFileCaseOnly = (QString::compare(cleanFrom, QDir::cleanPath(to), Qt::CaseInsensitive) == 0);
    if (!sameFileCaseOnly && QFileInfo::exists(to)) {
        setError(tr("Path already exists: %1").arg(to));
        return;
    }
    const QString newName = name.trimmed();
    movePathAsync(from, to, [this, from, to, newName](bool ok) {
        if (!ok) {
            setError(tr("Could not rename: %1").arg(from));
            return;
        }
        emit pathRenamed(from, to);
        refresh();
        setError(QString());
        emit operationCompleted(tr("Renamed to %1").arg(newName));
    });
}

void FileTreeModel::deletePath(const QString &path) {
    if (path.isEmpty() || !pathIsInsideRoot(path)) {
        setError(tr("Path is outside the project root."));
        return;
    }
    const QString cleanPath = QDir::cleanPath(path);
    const QString cleanRoot = QDir::cleanPath(m_rootPath);
    if (cleanPath == cleanRoot) {
        return;
    }
    const QString trashRoot = m_trashRootForTesting.isEmpty()
        ? defaultTrashRoot()
        : m_trashRootForTesting;
    if (!QDir().mkpath(trashRoot)) {
        setError(tr("Could not create the internal trash directory: %1")
                     .arg(QDir::toNativeSeparators(trashRoot)));
        return;
    }
    const QString stamp = QDateTime::currentDateTimeUtc().toString(QStringLiteral("yyyyMMddhhmmsszzz"));
    const QString baseName = QFileInfo(path).fileName();
    // The timestamp alone collides for two same-named items deleted in the
    // same millisecond; a unique suffix keeps every undo entry pointing at its
    // own copy.
    QString target;
    for (int attempt = 0; attempt < 1000; ++attempt) {
        const QString unique = QUuid::createUuid().toString(QUuid::WithoutBraces).left(8);
        const QString candidate = QDir(trashRoot).filePath(
            QStringLiteral("%1_%2_%3").arg(stamp, unique, baseName));
        if (!QFileInfo::exists(candidate)) {
            target = candidate;
            break;
        }
    }
    if (target.isEmpty()) {
        setError(tr("Could not allocate a unique trash path for: %1")
                     .arg(QDir::toNativeSeparators(path)));
        return;
    }

    movePathAsync(path, target, [this, path, target](bool ok) {
        if (!ok) {
            setError(tr("Could not move item to trash: %1").arg(path));
            return;
        }
        m_deleteStack.push_back({path, target});
        emit pathDeleted(path);
        refresh();
        setError(QString());
        emit canUndoChanged();
        emit operationCompleted(tr("Moved to internal trash - undo available"));
    });
}

void FileTreeModel::undoLastDelete() {
    if (m_deleteStack.isEmpty()) {
        setError(tr("Nothing to undo."));
        return;
    }
    const auto op = m_deleteStack.constLast();
    if (QFileInfo::exists(op.originalPath)) {
        setError(tr("Original path already exists: %1").arg(op.originalPath));
        return;
    }
    movePathAsync(op.trashPath, op.originalPath, [this, op](bool ok) {
        if (!ok) {
            setError(tr("Could not restore: %1").arg(op.originalPath));
            return;
        }
        if (!m_deleteStack.isEmpty()
            && m_deleteStack.constLast().trashPath == op.trashPath) {
            m_deleteStack.removeLast();
            emit canUndoChanged();
        }
        emit pathRestored(op.originalPath);
        refresh();
        setError(QString());
        emit operationCompleted(tr("Delete undone"));
    });
}

QVector<FileTreeModel::Node> FileTreeModel::readChildrenForRoot(
    const QString &path, int depth, const QString &rootPath) {
    QVector<Node> children;
    QDir dir(path);
    const auto entries = dir.entryInfoList(
        QDir::Dirs | QDir::Files | QDir::NoDotAndDotDot | QDir::Readable,
        QDir::DirsFirst | QDir::Name | QDir::IgnoreCase);

    const QString rootKey = containmentKey(rootPath);
    for (const auto &entry : entries) {
        // Skip symlink / junction targets that leave the project root.
        if (entry.fileName() == QStringLiteral(".git") || !isSameOrInside(rootKey, containmentKey(entry))) {
            continue;
        }
        children.push_back({
            entry.fileName(),
            entry.absoluteFilePath(),
            entry.isDir(),
            false,
            false,
            depth,
            false,
        });
    }
    return children;
}

int FileTreeModel::descendantEnd(int row) const {
    const int depth = m_nodes.at(row).depth;
    int end = row;
    for (int i = row + 1; i < m_nodes.size(); ++i) {
        if (m_nodes.at(i).depth <= depth) {
            break;
        }
        end = i;
    }
    return end;
}

bool FileTreeModel::pathIsInsideRoot(const QString &path) const {
    if (m_rootPath.isEmpty() || path.trimmed().isEmpty()) {
        return false;
    }
    // m_rootPath is canonical. A path lexically under it can only leave the
    // project through a link on the way down, so only then is it resolved; a
    // path spelled outside it (another alias of the root) is resolved too.
    const QString clean = QDir::cleanPath(QFileInfo(path).absoluteFilePath());
    if (!isSameOrInside(m_rootPath, clean)) return isSameOrInside(m_rootPath, containmentKey(clean));
    for (QString current = clean; current.size() > m_rootPath.size() && isSameOrInside(m_rootPath, current);
         current = QFileInfo(current).path()) {
        if (isLink(QFileInfo(current))) return isSameOrInside(m_rootPath, containmentKey(clean));
    }
    return true;
}

void FileTreeModel::loadChildren(int row) {
    if (row < 0 || row >= m_nodes.size())
        return;
    const QString path = m_nodes.at(row).path;
    const int depth = m_nodes.at(row).depth + 1;
    const QString rootPath = m_rootPath;
    const quint64 generation = m_loadGeneration;
    const quint64 token = ++m_nodes[row].loadToken;
    beginAsyncLoad();
    auto *watcher = new QFutureWatcher<QVector<Node>>(this);
    connect(watcher, &QFutureWatcher<QVector<Node>>::finished, this,
            [this, watcher, path, generation, token]() {
        const QVector<Node> children = watcher->result();
        watcher->deleteLater();
        if (generation == m_loadGeneration) {
            int row = -1;
            for (int i = 0; i < m_nodes.size(); ++i) {
                if (m_nodes.at(i).path == path) {
                    row = i;
                    break;
                }
            }
            if (row >= 0 && m_nodes.at(row).loadToken == token
                && m_nodes.at(row).expanded && !m_nodes.at(row).loaded) {
                if (!children.isEmpty()) {
                    beginInsertRows({}, row + 1, row + children.size());
                    insertNodes(row + 1, children.cbegin(), children.cend());
                    endInsertRows();
                }
                m_nodes[row].loaded = true;
                m_nodes[row].loading = false;
                applyKeepMounted(row, children.size());
                emit dataChanged(index(row), index(row), {LoadingRole});
                syncWatchedDirectories();
            } else if (row >= 0 && m_nodes.at(row).loadToken == token) {
                m_nodes[row].loading = false;
                emit dataChanged(index(row), index(row), {LoadingRole});
            }
        }
        endAsyncLoad();
    });
    watcher->setFuture(QtConcurrent::run([path, depth, rootPath]() {
        return readChildrenForRoot(path, depth, rootPath);
    }));
}

void FileTreeModel::beginAsyncLoad() {
    ++m_pendingAsyncLoads;
    if (m_pendingAsyncLoads == 1)
        setBusy(true);
}

void FileTreeModel::endAsyncLoad() {
    m_pendingAsyncLoads = qMax(0, m_pendingAsyncLoads - 1);
    if (m_pendingAsyncLoads == 0)
        setBusy(false);
}

void FileTreeModel::setError(const QString &message) {
    if (m_errorMessage == message) {
        return;
    }
    m_errorMessage = message;
    if (m_busy && !message.trimmed().isEmpty())
        m_progressFailed = true;
    emit errorMessageChanged();
}

void FileTreeModel::setBusy(bool value) {
    if (m_busy == value)
        return;
    m_busy = value;
    if (value) {
        m_progressFailed = false;
        emit progressStarted(m_progressTitle.isEmpty()
                                 ? tr("Loading files")
                                 : m_progressTitle);
        emit progressChanged(-1, {});
    }
    emit busyChanged();
    if (!value) {
        emit progressFinished(
            m_progressFailed ? span::core::OperationOutcome::Failed
                             : span::core::OperationOutcome::Succeeded,
            m_progressFailed ? m_errorMessage : QString());
        m_progressTitle.clear();
    }
}

QSet<QString> FileTreeModel::mountedDirectoryPaths() const {
    QSet<QString> paths;
    for (const Node &node : m_nodes) {
        if (node.isDir && node.loaded && (node.expanded || node.keepMounted))
            paths.insert(node.path);
    }
    return paths;
}

void FileTreeModel::applyKeepMounted(int row, int childCount) {
    if (row < 0 || row >= m_nodes.size())
        return;
    m_nodes[row].keepMounted = childCount <= kMountedChildLimit;
}

void FileTreeModel::collapseRemovingDescendants(int row) {
    m_nodes[row].loaded = false;
    m_nodes[row].keepMounted = false;
    ++m_nodes[row].loadToken;
    const int end = descendantEnd(row);
    if (end > row) {
        beginRemoveRows({}, row + 1, end);
        m_nodes.erase(m_nodes.begin() + row + 1, m_nodes.begin() + end + 1);
        endRemoveRows();
    }
}

void FileTreeModel::updateDescendantReveal(int parentRow) {
    const int end = descendantEnd(parentRow);
    if (end <= parentRow)
        return;
    computeReveal(m_nodes, parentRow, end);
    emit dataChanged(index(parentRow + 1), index(end), {RevealedRole});
}

void FileTreeModel::computeReveal(QVector<Node> &nodes, int parentRow, int end) {
    // A row is revealed when its parent is expanded and revealed. Rows are in
    // pre-order, so a stack of open ancestors finds each parent in one pass.
    QVector<int> ancestors{parentRow};
    for (int i = parentRow + 1; i <= end; ++i) {
        while (nodes.at(ancestors.constLast()).depth >= nodes.at(i).depth)
            ancestors.removeLast();
        const Node &parent = nodes.at(ancestors.constLast());
        nodes[i].revealed = parent.expanded && parent.revealed;
        ancestors.push_back(i);
    }
}

QVector<FileTreeModel::Node> FileTreeModel::buildMountedTree(
    const QString &rootPath,
    const QSet<QString> &expanded,
    const QSet<QString> &mounted) {
    QVector<Node> nodes;
    const QFileInfo info(rootPath);
    Node root;
    root.name = info.fileName().isEmpty() ? rootPath : info.fileName();
    root.path = rootPath;
    root.isDir = true;
    root.expanded = expanded.contains(rootPath);
    root.isRoot = true;
    root.revealed = true;
    root.depth = 0;
    nodes.push_back(root);

    // Pre-order append: each child goes to the end, followed by its own
    // subtree, so no row is ever inserted in the middle.
    const auto appendChildren = [&](auto &&self, int parentRow) -> void {
        const QString path = nodes.at(parentRow).path;
        const int depth = nodes.at(parentRow).depth + 1;
        const QVector<Node> children = readChildrenForRoot(path, depth, rootPath);
        const bool keepMounted = children.size() <= kMountedChildLimit;
        const bool expandedParent = nodes.at(parentRow).expanded;
        const bool wantMounted = mounted.contains(path);
        if (!expandedParent && !(wantMounted && keepMounted)) {
            nodes[parentRow].loaded = false;
            nodes[parentRow].keepMounted = false;
            return;
        }
        nodes[parentRow].loaded = true;
        nodes[parentRow].keepMounted = keepMounted;
        for (const Node &child : children) {
            nodes.push_back(child);
            if (!child.isDir) continue;
            const int childRow = nodes.size() - 1;
            nodes[childRow].expanded = expanded.contains(child.path);
            if (nodes.at(childRow).expanded || mounted.contains(child.path)) self(self, childRow);
        }
    };

    if (nodes[0].expanded || mounted.contains(rootPath))
        appendChildren(appendChildren, 0);

    computeReveal(nodes, 0, nodes.size() - 1);
    return nodes;
}

bool FileTreeModel::sameTreeStructure(const QVector<Node> &left,
                                     const QVector<Node> &right) {
    if (left.size() != right.size())
        return false;
    for (int i = 0; i < left.size(); ++i) {
        const Node &a = left.at(i);
        const Node &b = right.at(i);
        if (a.path != b.path || a.name != b.name || a.isDir != b.isDir
            || a.depth != b.depth || a.expanded != b.expanded
            || a.revealed != b.revealed)
            return false;
    }
    return true;
}

void FileTreeModel::insertNodes(int row, QVector<Node>::const_iterator first, QVector<Node>::const_iterator last)
{
    // One shift of the tail for the whole run instead of one per node.
    const qsizetype count = last - first;
    m_nodes.insert(row, count, Node());
    std::copy(first, last, m_nodes.begin() + row);
}

void FileTreeModel::applyRebuiltTree(QVector<Node> nodes) {
    if (sameTreeStructure(m_nodes, nodes)) {
        for (int i = 0; i < m_nodes.size(); ++i)
            m_nodes[i].keepMounted = nodes.at(i).keepMounted;
        syncWatchedDirectories();
        return;
    }
    // Both lists are pre-order with unique paths. Walk them together: keep
    // rows present in both, remove old runs the rebuild dropped, insert new
    // runs, so views keep their delegates and scroll position.
    QSet<QString> pendingOld;
    for (const Node &node : std::as_const(m_nodes)) pendingOld.insert(node.path);
    QSet<QString> rebuilt;
    for (const Node &node : std::as_const(nodes)) rebuilt.insert(node.path);

    const auto removeRows = [this, &pendingOld](int first, int last) {
        beginRemoveRows({}, first, last);
        for (int i = first; i <= last; ++i) pendingOld.remove(m_nodes.at(i).path);
        m_nodes.erase(m_nodes.begin() + first, m_nodes.begin() + last + 1);
        endRemoveRows();
    };

    int row = 0;
    int next = 0;
    while (next < nodes.size()) {
        const Node &target = nodes.at(next);
        if (row < m_nodes.size() && m_nodes.at(row).path == target.path) {
            Node &node = m_nodes[row];
            const bool visibleChange = node.name != target.name || node.isDir != target.isDir
                || node.depth != target.depth || node.expanded != target.expanded || node.revealed != target.revealed
                || node.loading != target.loading;
            // A load still in flight was planned against the old tree.
            const quint64 token = node.loadToken + 1;
            node = target;
            node.loadToken = token;
            pendingOld.remove(target.path);
            if (visibleChange) emit dataChanged(index(row), index(row));
            ++row;
            ++next;
            continue;
        }
        if (row < m_nodes.size() && !rebuilt.contains(m_nodes.at(row).path)) {
            int last = row;
            while (last + 1 < m_nodes.size() && !rebuilt.contains(m_nodes.at(last + 1).path)) ++last;
            removeRows(row, last);
            continue;
        }
        if (!pendingOld.contains(target.path)) {
            int last = next;
            while (last + 1 < nodes.size() && !pendingOld.contains(nodes.at(last + 1).path)) ++last;
            const int count = last - next + 1;
            beginInsertRows({}, row, row + count - 1);
            insertNodes(row, nodes.cbegin() + next, nodes.cbegin() + last + 1);
            endInsertRows();
            row += count;
            next = last + 1;
            continue;
        }
        // Both rows survive but their order changed: drop the old row; the
        // rebuilt one is inserted where the new order puts it.
        removeRows(row, row);
    }
    if (row < m_nodes.size()) removeRows(row, m_nodes.size() - 1);
    rebuildAggregateGitStatuses();
    syncWatchedDirectories();
}

void FileTreeModel::scheduleWatchedRefresh() {
    if (m_rootPath.isEmpty())
        return;
    m_watchCoalesce.start();
}

void FileTreeModel::flushWatchedRefresh() {
    if (m_rootPath.isEmpty())
        return;
    // Drops watches on directories that vanished, once per coalesced burst.
    syncWatchedDirectories();
    if (m_busy || m_pendingAsyncLoads > 0 || m_silentRebuild) {
        m_watchCoalesce.start();
        return;
    }
    refreshPreservingExpansion(false);
}

void FileTreeModel::onWatchedPathChanged(const QString &path) {
    if (m_rootPath.isEmpty() || shouldIgnoreWatchPath(path)) return;
    scheduleWatchedRefresh();
}

bool FileTreeModel::shouldIgnoreWatchPath(const QString &path) const {
    const QString normalized = QDir::fromNativeSeparators(path);
    if (normalized.endsWith(QLatin1String("/.git"))
        || normalized.contains(QLatin1String("/.git/")))
        return true;
    if (normalized == QDir::fromNativeSeparators(m_rootPath))
        return false;
    return !pathIsInsideRoot(path);
}

void FileTreeModel::syncWatchedDirectories() {
    QStringList wanted;
    QSet<QString> seen;
    if (!m_rootPath.isEmpty() && QFileInfo(m_rootPath).isDir()) {
        wanted.push_back(QDir::fromNativeSeparators(m_rootPath));
        seen.insert(wanted.constLast());
    }
    for (const Node &node : m_nodes) {
        if (!node.isDir || node.path.isEmpty())
            continue;
        if (!(node.expanded || node.keepMounted))
            continue;
        const QString path = QDir::fromNativeSeparators(node.path);
        if (!seen.contains(path) && QFileInfo(node.path).isDir()) {
            seen.insert(path);
            wanted.push_back(path);
        }
    }
    if (wanted.size() > kMaxWatchedDirectories) {
        // Stable, so equally shallow directories keep tree order.
        std::stable_sort(wanted.begin() + 1, wanted.end(), [](const QString &a, const QString &b) {
            return a.count(QLatin1Char('/')) < b.count(QLatin1Char('/'));
        });
        wanted = wanted.mid(0, kMaxWatchedDirectories);
    }
    const QSet<QString> wantedSet(wanted.cbegin(), wanted.cend());

    QSet<QString> current;
    for (const QString &path : m_watcher.directories())
        current.insert(QDir::fromNativeSeparators(path));

    QStringList toRemove;
    for (const QString &path : current) {
        if (!wantedSet.contains(path)) toRemove.push_back(path);
    }
    if (!toRemove.isEmpty())
        m_watcher.removePaths(toRemove);

    QSet<QString> remaining;
    for (const QString &path : m_watcher.directories())
        remaining.insert(QDir::fromNativeSeparators(path));
    QStringList toAdd;
    for (const QString &path : wanted) {
        if (!remaining.contains(path) && QFileInfo(path).isDir())
            toAdd.push_back(path);
    }
    if (!toAdd.isEmpty())
        m_watcher.addPaths(toAdd);
}
