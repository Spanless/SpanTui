#include "git/git_repository_service.h"

#include "core/operation_outcome.h"
#include "git/git_constants.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFutureWatcher>
#include <QHash>
#include <QMutex>
#include <QMutexLocker>
#include <QtConcurrent>

#include <algorithm>
#include <utility>

namespace {

// Hover previews are cheap to refetch; a small bound keeps a long scan from
// pinning every message it passed over.
constexpr qsizetype kMaximumCachedPreviews = 256;
// Paths per git process: bounds one process's output, and keeps a freshly
// expanded file from waiting behind a long expand-all backlog.
constexpr int kDiffBodyBatch = 16;
// Requests left behind by rows that scrolled away are dropped past this.
constexpr int kDiffBodyQueueLimit = 64;
// Worktree bodies kept fresh across refreshes; older ones are forgotten.
constexpr int kWorktreeRefetchLimit = 24;
// Untracked files are read straight from disk, up to this size.
constexpr qint64 kUntrackedReadLimit = qint64(2) * 1024 * 1024;
// Bodies of unchanged untracked files kept across refreshes; past this the
// cache keeps only line counts.
constexpr qsizetype kUntrackedBodyBudget = qsizetype(16) * 1024 * 1024;
// A NUL in this many leading bytes marks a file binary.
constexpr qsizetype kBinarySniffBytes = 8192;

} // namespace

void GitRepositoryService::selectHistoryCommit(const QString &hash) {
    selectCommit(hash, true);
}

void GitRepositoryService::selectCommit(const QString &hash, bool reportBusy) {
    if (hash.isEmpty()) return;
    if (!isSafeGitObjectId(hash)) {
        setError(tr("Invalid commit id."));
        return;
    }
    m_selectedCommit = hash;
    m_selectedCommitDetails.clear();
    resetDiffBodies(DiffScope::Commit);
    m_commitDiffFiles.clear();
    m_selectedCommitDetails = m_history.itemAt(m_history.rowOfHash(hash));
    m_selectedParent = m_selectedCommitDetails.value(QStringLiteral("parents"))
                           .toStringList().value(0);
    emit selectedCommitChanged();
    followOnIfUnblocked(true, [this, reportBusy]() {
        loadSelectedCommitBody(reportBusy);
    });
}

QVariantMap GitRepositoryService::commitPreview(const QString &hash) const {
    return m_commitPreviews.value(hash);
}

void GitRepositoryService::requestCommitPreview(const QString &hash) {
    if (!m_available || !isSafeGitObjectId(hash) || m_commitPreviews.contains(hash))
        return;
    // `-m --first-parent` gives merges a first-parent shortstat; plain
    // `show` would print a combined diff with no totals.
    runGit({QStringLiteral("show"), QStringLiteral("--no-color"), QStringLiteral("-m"),
            QStringLiteral("--first-parent"), QStringLiteral("--shortstat"),
            QStringLiteral("--format=%B%x00"), hash},
           QStringLiteral("commit-preview"),
           [this, hash](bool ok, const QByteArray &out, const QByteArray &) {
        if (!ok)
            return;
        if (m_commitPreviews.size() >= kMaximumCachedPreviews)
            m_commitPreviews.clear();
        QVariantMap preview = parseCommitPreview(out);
        preview.insert(QStringLiteral("hash"), hash);
        m_commitPreviews.insert(hash, preview);
        emit commitPreviewReady(hash, preview);
    }, false);
}

void GitRepositoryService::loadSelectedCommitBody(bool reportBusy) {
    const QString hash = m_selectedCommit;
    if (hash.isEmpty()) return;
    runGit(
        {QStringLiteral("show"), QStringLiteral("-s"), QStringLiteral("--format=%b"), hash}, kGitOperationCommitDetails,
        [this, hash, reportBusy](bool ok, const QByteArray &out, const QByteArray &err) {
            if (!ok) {
                if (reportBusy) {
                    setError(cleanError(err));
                    m_git.setLaneBusy(GitLane::Diff, false, kGitOperationCommitDetails);
                }
                return;
            }
            if (m_selectedCommit != hash) {
                if (reportBusy) {
                    m_git.finishLaneTask(GitLane::Diff, kGitOperationCommitDetails,
                                         span::core::OperationOutcome::Cancelled);
                }
                return;
            }
            m_selectedCommitDetails.insert(QStringLiteral("body"), QString::fromUtf8(out).trimmed());
            emit selectedCommitChanged();
            if (m_git.refreshQueued()) {
                if (reportBusy) m_git.setLaneBusy(GitLane::Diff, false, kGitOperationCommitDetails);
                scheduleRefresh(true);
                return;
            }
            loadSelectedCommitFiles(reportBusy);
        },
        reportBusy);
}

void GitRepositoryService::loadSelectedCommitFiles(bool reportBusy) {
    if (m_selectedCommit.isEmpty()) return;
    if (reportBusy)
        m_commitDiffFiles.clear();
    const QString hash = m_selectedCommit;
    const QString parentHash = m_selectedParent;

    // Stage 1: Fast metadata via name-status (instantaneous even for thousands of files)
    QStringList nameStatusArgs;
    if (parentHash.isEmpty()) {
        nameStatusArgs = {QStringLiteral("show"), QStringLiteral("--name-status"),
                          QStringLiteral("-z"), QStringLiteral("--format="),
                          QStringLiteral("-M"), hash};
    } else {
        nameStatusArgs = {QStringLiteral("diff"), QStringLiteral("--name-status"),
                          QStringLiteral("-z"), QStringLiteral("-M"),
                          parentHash, hash};
    }

    runGit(
        nameStatusArgs, kGitOperationCommitFiles,
        [this, hash, parentHash, reportBusy](bool ok, const QByteArray &nameStatusOut, const QByteArray &err) {
            if (!ok) {
                if (reportBusy) {
                    setError(cleanError(err));
                    m_git.finishLaneTask(GitLane::Diff, kGitOperationCommitFiles, span::core::OperationOutcome::Failed,
                                         cleanError(err));
                }
                return;
            }
            if (m_selectedCommit != hash || m_selectedParent != parentHash) {
                if (reportBusy) {
                    m_git.finishLaneTask(GitLane::Diff, kGitOperationCommitFiles,
                                         span::core::OperationOutcome::Cancelled);
                }
                return;
            }

            QVariantList entries = parseNameStatus(nameStatusOut);

            // Stage 2: line counts and binary flags via numstat. Diff bodies are
            // not part of the list; views request them per file on demand.
            QStringList numstatArgs;
            if (parentHash.isEmpty()) {
                numstatArgs = {QStringLiteral("show"), QStringLiteral("--numstat"), QStringLiteral("--format="),
                               QStringLiteral("-M"), hash};
            } else {
                numstatArgs = {QStringLiteral("diff"), QStringLiteral("--numstat"), QStringLiteral("-M"), parentHash,
                               hash};
            }

            runGit(
                numstatArgs, kGitOperationCommitFilesStats,
                [this, hash, parentHash, reportBusy, entries](bool okStats, const QByteArray &numstatOut,
                                                              const QByteArray &) mutable {
                    if (m_selectedCommit != hash || m_selectedParent != parentHash) {
                        if (reportBusy) {
                            m_git.finishLaneTask(GitLane::Diff, kGitOperationCommitFilesStats,
                                                 span::core::OperationOutcome::Cancelled);
                        }
                        return;
                    }

                    if (okStats) {
                        const auto stats = parseNumstat(numstatOut);
                        for (QVariant &itemVal : entries) {
                            QVariantMap map = itemVal.toMap();
                            const QString p = map.value(QStringLiteral("path")).toString();
                            if (stats.contains(p)) {
                                const auto stat = stats.value(p);
                                map.insert(QStringLiteral("additions"), stat.additions);
                                map.insert(QStringLiteral("deletions"), stat.deletions);
                                map.insert(QStringLiteral("binary"), stat.binary);
                                itemVal = map;
                            }
                        }
                    }

                    m_commitDiffFiles.replace(std::move(entries));
                    if (reportBusy) {
                        m_git.finishLaneTask(GitLane::Diff, kGitOperationCommitFilesStats,
                                             span::core::OperationOutcome::Succeeded);
                    }
                },
                reportBusy);
        },
        reportBusy);
}

namespace {

// Rows readUntrackedFile would show: binary and oversized files count none.
int countUntrackedLines(const QString &absolutePath) {
    QFile file(absolutePath);
    if (!file.open(QIODevice::ReadOnly) || file.size() > kUntrackedReadLimit)
        return 0;
    const QByteArray content = file.readAll();
    if (content.isEmpty() || content.left(kBinarySniffBytes).contains('\0')) return 0;
    return int(content.count('\n')) + (content.endsWith('\n') ? 0 : 1);
}

// The whole file as added rows.
GitDiffBodyPtr readUntrackedFile(const QString &absolutePath)
{
    auto body = QSharedPointer<GitDiffBody>::create();
    QFile file(absolutePath);
    if (!file.open(QIODevice::ReadOnly)) return body;
    if (file.peek(kBinarySniffBytes).contains('\0')) {
        body->binary = true;
        return body;
    }
    if (file.size() > kUntrackedReadLimit) {
        body->tooLarge = true;
        return body;
    }
    const QByteArray content = file.readAll();
    body->digest = qHash(content);
    QList<QByteArray> rows = content.split('\n');
    // A final newline ends the last row; it does not start another one.
    if (!rows.isEmpty() && rows.constLast().isEmpty()) rows.removeLast();
    int line = 1;
    for (QByteArray &row : rows) {
        if (row.endsWith('\r')) row.chop(1);
        ++body->additions;
        body->append(GitDiffLine::Add, 0, line++, QString::fromUtf8(row));
    }
    return body;
}

QStringList diffBodyOptions() {
    return {QStringLiteral("--no-ext-diff"), QStringLiteral("--no-color"),
            QStringLiteral("--unified=3"), QStringLiteral("-M")};
}

QSet<QString> modelPaths(const GitItemModel &model) {
    QSet<QString> paths;
    paths.reserve(model.count());
    for (const QVariant &value : model.items())
        paths.insert(value.toMap().value(QStringLiteral("path")).toString());
    return paths;
}
} // namespace

// Line counts and bodies of untracked files, reused while a file keeps its
// size and modification time: every status refresh recounts them and
// refetches the open bodies. Filled from worker threads.
struct GitRepositoryService::UntrackedFileCache {
    struct Entry {
        qint64 size = -1;
        QDateTime modified;
        int lines = -1;
        GitDiffBodyPtr body;
    };

    int lines(const QString &absolutePath)
    {
        const QFileInfo info(absolutePath);
        {
            QMutexLocker lock(&mutex);
            const auto it = entries.constFind(absolutePath);
            if (it != entries.cend() && matches(*it, info) && it->lines >= 0) return it->lines;
        }
        const int counted = countUntrackedLines(absolutePath);
        QMutexLocker lock(&mutex);
        entryFor(absolutePath, info).lines = counted;
        return counted;
    }

    GitDiffBodyPtr body(const QString &absolutePath)
    {
        const QFileInfo info(absolutePath);
        {
            QMutexLocker lock(&mutex);
            const auto it = entries.constFind(absolutePath);
            if (it != entries.cend() && matches(*it, info) && it->body) return it->body;
        }
        const GitDiffBodyPtr read = readUntrackedFile(absolutePath);
        QMutexLocker lock(&mutex);
        Entry &entry = entryFor(absolutePath, info);
        if (entry.body) bodyBytes -= entry.body->bytes;
        entry.body = read;
        entry.lines = read->binary || read->tooLarge ? 0 : read->additions;
        bodyBytes += read->bytes;
        if (bodyBytes > kUntrackedBodyBudget) {
            for (Entry &cached : entries) cached.body.reset();
            bodyBytes = 0;
        }
        return read;
    }

private:
    static bool matches(const Entry &entry, const QFileInfo &info)
    {
        return entry.size == info.size() && entry.modified == info.lastModified();
    }

    // The entry stamped with the file's current size and time; a changed
    // file starts over.
    Entry &entryFor(const QString &absolutePath, const QFileInfo &info)
    {
        Entry &entry = entries[absolutePath];
        if (!matches(entry, info)) {
            if (entry.body) bodyBytes -= entry.body->bytes;
            entry = Entry{info.size(), info.lastModified(), -1, {}};
        }
        return entry;
    }

    QMutex mutex;
    QHash<QString, Entry> entries;
    qsizetype bodyBytes = 0;
};

QSharedPointer<GitRepositoryService::UntrackedFileCache> GitRepositoryService::untrackedFileCache()
{
    if (!m_untrackedFiles) m_untrackedFiles = QSharedPointer<UntrackedFileCache>::create();
    return m_untrackedFiles;
}

bool GitRepositoryService::diffScopeFromName(const QString &name, DiffScope *scope)
{
    if (name == QLatin1String("changes")) {
        *scope = DiffScope::Changes;
        return true;
    }
    if (name == QLatin1String("commit")) {
        *scope = DiffScope::Commit;
        return true;
    }
    return false;
}

QString GitRepositoryService::diffScopeName(DiffScope scope)
{
    return scope == DiffScope::Commit ? QStringLiteral("commit") : QStringLiteral("changes");
}

GitItemModel &GitRepositoryService::diffScopeModel(DiffScope scope)
{
    return scope == DiffScope::Commit ? m_commitDiffFiles : m_commitChanges;
}

void GitRepositoryService::resetDiffBodies(DiffScope scope)
{
    DiffBodyQueue &bodies = m_diffBodies[int(scope)];
    ++bodies.epoch;
    bodies.queue.clear();
    bodies.solo.clear();
    bodies.waiting.clear();
    if (bodies.store.size() == 0)
        return;
    bodies.store.clear();
    emit diffLinesChanged(diffScopeName(scope), {});
}

void GitRepositoryService::requestDiffLines(const QString &scopeName, const QStringList &paths)
{
    DiffScope scope = DiffScope::Changes;
    if (!m_available || paths.isEmpty() || !diffScopeFromName(scopeName, &scope))
        return;
    if (scope == DiffScope::Commit && m_selectedCommit.isEmpty())
        return;
    // Only rows of the list are served: the paths reach git after "--".
    const QSet<QString> listed = modelPaths(diffScopeModel(scope));
    const GitDiffStore &store = m_diffBodies[int(scope)].store;
    QStringList wanted;
    for (const QString &path : paths) {
        if (listed.contains(path) && !store.contains(path))
            wanted.append(path);
    }
    enqueueDiffBodies(scope, wanted);
}

QVariant GitRepositoryService::diffLines(const QString &scopeName, const QString &path)
{
    DiffScope scope = DiffScope::Changes;
    if (!diffScopeFromName(scopeName, &scope))
        return {};
    const GitDiffBodyPtr body = m_diffBodies[int(scope)].store.find(path);
    return body ? QVariant(body->toVariant()) : QVariant();
}

void GitRepositoryService::enqueueDiffBodies(DiffScope scope, const QStringList &paths)
{
    DiffBodyQueue &bodies = m_diffBodies[int(scope)];
    for (const QString &path : paths) {
        if (path.isEmpty() || bodies.waiting.contains(path))
            continue;
        bodies.waiting.insert(path);
        bodies.queue.append(path);
    }
    // Rows that scrolled away leave stale requests behind. Keep the newest;
    // views still showing a dropped path hear about it and ask again.
    QStringList dropped;
    while (bodies.queue.size() > kDiffBodyQueueLimit) {
        const QString path = bodies.queue.takeFirst();
        bodies.waiting.remove(path);
        dropped.append(path);
    }
    pumpDiffBodies();
    if (!dropped.isEmpty())
        emit diffLinesChanged(diffScopeName(scope), dropped);
}

void GitRepositoryService::pumpDiffBodies()
{
    if (m_diffBodyInFlight || !m_available)
        return;
    // Alternate between the two lists so a backlog in one cannot starve the
    // other. Newest requests go first: they are the rows now on screen.
    for (int step = 0; step < int(DiffScope::Count); ++step) {
        const int index = (m_diffBodyNextScope + step) % int(DiffScope::Count);
        DiffBodyQueue &bodies = m_diffBodies[index];
        QStringList paths;
        if (!bodies.solo.isEmpty()) {
            paths.append(bodies.solo.takeLast());
        } else if (!bodies.queue.isEmpty()) {
            const qsizetype count = std::min<qsizetype>(bodies.queue.size(), kDiffBodyBatch);
            paths = bodies.queue.mid(bodies.queue.size() - count);
            bodies.queue.remove(bodies.queue.size() - count, count);
        } else {
            continue;
        }
        m_diffBodyNextScope = (index + 1) % int(DiffScope::Count);
        startDiffBodyFetch(DiffScope(index), paths);
        return;
    }
}

void GitRepositoryService::startDiffBodyFetch(DiffScope scope, const QStringList &paths)
{
    if (scope == DiffScope::Changes && m_headUnborn) {
        // No HEAD: every file is new in full. The index and worktree diffs
        // each hold only part of a file staged and then edited again.
        const quint64 serial = ++m_diffBodyRequest;
        m_diffBodyInFlight = true;
        parseDiffBodies(scope, paths, serial, m_diffBodies[int(scope)].epoch, {});
        return;
    }
    // A renamed row also names its old path, so rename detection can still
    // pair both sides under the pathspec.
    QStringList pathspec = paths;
    const QSet<QString> requested(paths.cbegin(), paths.cend());
    for (const QVariant &value : diffScopeModel(scope).items()) {
        const QVariantMap item = value.toMap();
        const QString oldPath = item.value(QStringLiteral("oldPath")).toString();
        if (!oldPath.isEmpty() && requested.contains(item.value(QStringLiteral("path")).toString()))
            pathspec.append(oldPath);
    }

    QStringList args;
    if (scope == DiffScope::Changes) {
        args = QStringList{QStringLiteral("diff"), QStringLiteral("HEAD")} + diffBodyOptions();
    } else if (m_selectedParent.isEmpty()) {
        args = QStringList{QStringLiteral("show"), QStringLiteral("--format=")}
               + diffBodyOptions();
        args.append(m_selectedCommit);
    } else {
        args = QStringList{QStringLiteral("diff")} + diffBodyOptions();
        args << m_selectedParent << m_selectedCommit;
    }
    args.append(QStringLiteral("--"));
    args += pathspec;

    const quint64 serial = ++m_diffBodyRequest;
    const quint64 epoch = m_diffBodies[int(scope)].epoch;
    m_diffBodyInFlight = true;
    const bool started = runGit(args, QStringLiteral("diff-body"),
        [this, scope, paths, serial, epoch](bool ok, const QByteArray &out,
                                            const QByteArray &err) {
            finishDiffBodyFetch(scope, paths, serial, epoch, ok, out, err);
        }, false);
    if (!started) {
        m_diffBodyInFlight = false;
        for (const QString &path : paths)
            m_diffBodies[int(scope)].waiting.remove(path);
    }
}

void GitRepositoryService::finishDiffBodyFetch(DiffScope scope, const QStringList &paths,
                                               quint64 serial, quint64 epoch, bool ok,
                                               const QByteArray &out, const QByteArray &err)
{
    if (serial != m_diffBodyRequest)
        return;
    DiffBodyQueue &bodies = m_diffBodies[int(scope)];
    if (ok && epoch == bodies.epoch) {
        // The pump stays held until the parsed bodies are applied.
        parseDiffBodies(scope, paths, serial, epoch, out);
        return;
    }
    m_diffBodyInFlight = false;
    if (epoch == bodies.epoch) {
        const bool overLimit = m_git.outputLimitExceeded(GitLane::DiffBody);
        if (overLimit && paths.size() > 1) {
            // Retry one file per process so only the oversized file fails.
            bodies.solo += paths;
        } else if (overLimit) {
            failDiffBodies(scope, paths, true, {});
        } else {
            failDiffBodies(scope, paths, false, cleanError(err));
        }
    }
    pumpDiffBodies();
}

QList<QPair<QString, QString>> GitRepositoryService::diskBodyPaths(DiffScope scope, const QStringList &paths) const
{
    QList<QPair<QString, QString>> result;
    if (scope != DiffScope::Changes) return result;
    const QSet<QString> requested(paths.cbegin(), paths.cend());
    for (const QVariant &value : m_commitChanges.items()) {
        const QVariantMap item = value.toMap();
        const QString path = item.value(QStringLiteral("path")).toString();
        // `git diff HEAD` skips untracked files, and without a HEAD there is
        // nothing to diff against: those bodies come from disk.
        if (!requested.contains(path)
            || (!m_headUnborn && item.value(QStringLiteral("status")).toString() != QLatin1String("added")))
            continue;
        QString absolute;
        if (resolveWorktreePath(path, &absolute, nullptr)) result.append({path, absolute});
    }
    return result;
}

void GitRepositoryService::parseDiffBodies(DiffScope scope, const QStringList &paths, quint64 serial, quint64 epoch,
                                           const QByteArray &out)
{
    using Parsed = QPair<QList<GitDiffFile>, QHash<QString, GitDiffBodyPtr>>;
    const QList<QPair<QString, QString>> diskPaths = diskBodyPaths(scope, paths);
    const auto cache = untrackedFileCache();
    auto *watcher = new QFutureWatcher<Parsed>(this);
    connect(watcher, &QFutureWatcher<Parsed>::finished, this, [this, watcher, scope, paths, serial, epoch]() {
        const Parsed parsed = watcher->result();
        watcher->deleteLater();
        // A repository switch bumped the serial and already freed the pump;
        // a reset list bumped the epoch.
        if (serial != m_diffBodyRequest) return;
        m_diffBodyInFlight = false;
        if (epoch == m_diffBodies[int(scope)].epoch) applyDiffBodies(scope, paths, parsed.first, parsed.second);
        pumpDiffBodies();
    });
    watcher->setFuture(QtConcurrent::run([out, diskPaths, cache]() {
        Parsed parsed;
        if (!out.isEmpty()) parsed.first = parseUnifiedDiffFiles(out);
        QSet<QString> diffed;
        for (const GitDiffFile &file : parsed.first) diffed.insert(file.item.value(QStringLiteral("path")).toString());
        for (const auto &file : diskPaths) {
            if (!diffed.contains(file.first)) parsed.second.insert(file.first, cache->body(file.second));
        }
        return parsed;
    }));
}

void GitRepositoryService::applyDiffBodies(DiffScope scope, const QStringList &paths, const QList<GitDiffFile> &files,
                                           const QHash<QString, GitDiffBodyPtr> &diskBodies)
{
    DiffBodyQueue &bodies = m_diffBodies[int(scope)];
    GitItemModel &model = diffScopeModel(scope);
    QHash<QString, const GitDiffFile *> parsed;
    for (const GitDiffFile &file : files)
        parsed.insert(file.item.value(QStringLiteral("path")).toString(), &file);
    const QSet<QString> requested(paths.cbegin(), paths.cend());
    for (const QString &path : paths)
        bodies.waiting.remove(path);

    QVariantList items = model.items();
    bool itemsChanged = false;
    QStringList changed;
    for (QVariant &value : items) {
        const QVariantMap before = value.toMap();
        const QString path = before.value(QStringLiteral("path")).toString();
        if (!requested.contains(path))
            continue;
        QVariantMap item = before;
        GitDiffBodyPtr body;
        if (const GitDiffFile *file = parsed.value(path)) {
            // The parsed diff is authoritative for the row's counts, binary
            // flag and rename source.
            body = file->body;
            for (const QString &key : {QStringLiteral("additions"), QStringLiteral("deletions"),
                                       QStringLiteral("binary")})
                item.insert(key, file->item.value(key));
            const QString oldPath = file->item.value(QStringLiteral("oldPath")).toString();
            if (!oldPath.isEmpty())
                item.insert(QStringLiteral("oldPath"), oldPath);
        } else if (const GitDiffBodyPtr disk = diskBodies.value(path)) {
            body = disk;
            item.insert(QStringLiteral("additions"), body->additions);
            item.insert(QStringLiteral("deletions"), 0);
            item.insert(QStringLiteral("binary"), body->binary);
        } else {
            body = GitDiffBodyPtr(QSharedPointer<GitDiffBody>::create());
        }
        if (item != before) {
            value = item;
            itemsChanged = true;
        }
        if (bodies.store.insert(path, body))
            changed.append(path);
    }
    if (itemsChanged)
        model.replace(std::move(items));
    if (!changed.isEmpty())
        emit diffLinesChanged(diffScopeName(scope), changed);
}

void GitRepositoryService::failDiffBodies(DiffScope scope, const QStringList &paths,
                                          bool tooLarge, const QString &error)
{
    DiffBodyQueue &bodies = m_diffBodies[int(scope)];
    QStringList changed;
    for (const QString &path : paths) {
        bodies.waiting.remove(path);
        auto body = QSharedPointer<GitDiffBody>::create();
        body->tooLarge = tooLarge;
        if (!tooLarge)
            body->error = error.isEmpty() ? tr("Git could not produce this diff.") : error;
        if (bodies.store.insert(path, body))
            changed.append(path);
    }
    if (!changed.isEmpty())
        emit diffLinesChanged(diffScopeName(scope), changed);
}

void GitRepositoryService::countUntrackedStats(int additions, int deletions,
                                               QHash<QString, QPair<int, int>> perFileStats)
{
    const quint64 request = ++m_untrackedStatsRequest;
    QList<QPair<QString, QString>> files;
    for (const QVariant &value : m_commitChanges.items()) {
        const QVariantMap item = value.toMap();
        const QString path = item.value(QStringLiteral("path")).toString();
        // Without a commit every file is counted from disk. A collapsed
        // untracked directory has no single file to count.
        if ((!m_headUnborn && !item.value(QStringLiteral("untracked")).toBool())
            || path.endsWith(QLatin1Char('/'))
            || perFileStats.contains(path))
            continue;
        QString absolute;
        if (resolveWorktreePath(path, &absolute, nullptr))
            files.append({path, absolute});
    }
    if (files.isEmpty())
        return;
    const quint64 generation = m_git.generation();
    const auto cache = untrackedFileCache();
    using Counts = QHash<QString, int>;
    auto *watcher = new QFutureWatcher<Counts>(this);
    connect(watcher, &QFutureWatcher<Counts>::finished, this,
            [this, watcher, request, generation, additions, deletions, perFileStats]() mutable {
        const Counts counted = watcher->result();
        watcher->deleteLater();
        if (request != m_untrackedStatsRequest || generation != m_git.generation())
            return;
        for (auto it = counted.cbegin(); it != counted.cend(); ++it) {
            additions += it.value();
            perFileStats.insert(it.key(), {it.value(), 0});
        }
        setDiffStats(additions, deletions, perFileStats);
    });
    watcher->setFuture(QtConcurrent::run([files, cache]() {
        Counts counted;
        for (const auto &file : files) counted.insert(file.first, cache->lines(file.second));
        return counted;
    }));
}

void GitRepositoryService::refreshWorktreeDiffBodies()
{
    DiffBodyQueue &bodies = m_diffBodies[int(DiffScope::Changes)];
    if (bodies.store.size() == 0)
        return;
    const QSet<QString> listed = modelPaths(m_commitChanges);
    const QStringList recent = bodies.store.paths();
    QStringList refetch;
    QStringList dropped;
    for (qsizetype i = recent.size() - 1; i >= 0; --i) {
        const QString &path = recent.at(i);
        if (listed.contains(path) && refetch.size() < kWorktreeRefetchLimit) {
            refetch.prepend(path);
        } else {
            bodies.store.remove(path);
            dropped.append(path);
        }
    }
    // Refetched bodies keep serving until the new content lands; a view is
    // told only when it actually changed.
    enqueueDiffBodies(DiffScope::Changes, refetch);
    if (!dropped.isEmpty())
        emit diffLinesChanged(diffScopeName(DiffScope::Changes), dropped);
}
