#include "git/git_repository_service.h"

#include "git/git_constants.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFutureWatcher>
#include <QLatin1String>
#include <QVariantMap>
#include <QtConcurrent>

#include <functional>
#include <utility>

// Repository reads: status, diff stats, branches and the paged history
// load, plus the watcher and poll that schedule them. Processes run through
// GitLaneScheduler.
namespace {
struct HistoryPage {
    QVariantList rows;
    GitRepositoryService::HistoryGraph graph;
};

// Row fields the graph draws.
bool sameGraphRow(const QVariantMap &a, const QVariantMap &b)
{
    for (const char *key : {"hash", "parents", "nodeLane", "nodeColor", "laneCount", "graphSegments"}) {
        const QString name = QLatin1String(key);
        if (a.value(name) != b.value(name)) return false;
    }
    return true;
}

// Commit a ref decorates; an annotated tag decorates the commit it peels to.
QString refCommit(const QVariantMap &ref)
{
    const QString peeled = ref.value(QStringLiteral("commitId")).toString();
    return peeled.isEmpty() ? ref.value(QStringLiteral("objectId")).toString() : peeled;
}
} // namespace

bool GitRepositoryService::runGit(const QStringList &arguments, const QString &operation,
                                  Completion completion, bool reportBusy) {
    return runGitAt(m_repositoryRoot, arguments, operation, std::move(completion), reportBusy);
}

bool GitRepositoryService::runGitAt(const QString &directory, const QStringList &arguments,
                                    const QString &operation, Completion completion,
                                    bool reportBusy) {
    if (directory.isEmpty())
        return false;
    const GitLane lane = GitLaneScheduler::laneForOperation(operation);
    // Per-file body fetches follow scrolling and worktree probes read other
    // worktrees; neither may hold off refreshes.
    if (lane != GitLane::Status && lane != GitLane::DiffBody && lane != GitLane::Probe) m_refreshCoalesce.stop();
    return m_git.run(directory, arguments, operation, std::move(completion), reportBusy);
}

void GitRepositoryService::followOnIfUnblocked(bool includeBranches,
                                              const std::function<void()> &startNext) {
    if (m_git.refreshQueued()) {
        scheduleRefresh(includeBranches);
        return;
    }
    startNext();
}

void GitRepositoryService::refreshStatus(bool includeBranches, bool reportBusy) {
    // -uall lists every untracked file (a new directory is not one opaque
    // row); --ignored=matching keeps an ignored directory a single entry —
    // "traditional" would list every file inside build/ once -uall is on.
    // Edits landing after this point mark the tree again for the next poll.
    m_workTreeDirty = false;
    runGit(
        {QStringLiteral("status"), QStringLiteral("--porcelain=v2"), QStringLiteral("-z"), QStringLiteral("--branch"),
         QStringLiteral("--untracked-files=all"), QStringLiteral("--ignored=matching")},
        kGitOperationRefresh,
        [this, includeBranches, reportBusy](bool ok, const QByteArray &out, const QByteArray &err) {
            if (!ok) {
                m_workTreeDirty = true;
                if (reportBusy) setError(cleanError(err));
                return;
            }
            QVariantList staged, working;
            QHash<QString, QString> statuses;
            const QVariantMap state = parseStatus(out, &staged, &working, &statuses);
            const QString branch = state.value(QStringLiteral("branch")).toString();
            const bool repositoryStateChanged = branch != m_currentBranch;
            const bool statusesChanged = statuses != m_statuses;
            m_currentBranch = branch;
            m_headUnborn = state.value(QStringLiteral("unborn")).toBool();
            m_lastHeadIdentity = headIdentity();
            rebuildCommitChanges(staged, working);
            m_statuses = std::move(statuses);
            // Idle polls only keep the status map fresh; diff stats and diff
            // lines are recomputed by the wake refresh
            // (setIdle(false) -> scheduleRefresh).
            if (!m_idle) refreshDiffStats();
            if (repositoryStateChanged) emit repositoryChanged();
            if (statusesChanged) emit statusMapChanged();
            if (includeBranches) {
                followOnIfUnblocked(true, [this, reportBusy]() { refreshBranches(reportBusy); });
            }
        },
        reportBusy);
}
void GitRepositoryService::refreshDiffStats() {
    // Without a commit every listed file is new in full: count the
    // working-tree files the way their bodies are read, not the index and
    // worktree diffs, which each hold only part of a file that was staged
    // and then edited again.
    if (m_headUnborn) {
        setDiffStats(0, 0, {});
        countUntrackedStats(0, 0, {});
        refreshWorktreeDiffBodies();
        return;
    }
    const bool started = runGit({QStringLiteral("diff"), QStringLiteral("HEAD"), QStringLiteral("--numstat")},
           QStringLiteral("diff-stats"),
           [this](bool ok, const QByteArray &out, const QByteArray &) {
        int add = 0, del = 0;
        QHash<QString, QPair<int, int>> stats;
        if (ok) {
            const auto parsed = parseNumstat(out);
            for (auto it = parsed.cbegin(); it != parsed.cend(); ++it) {
                add += it.value().additions;
                del += it.value().deletions;
                stats[it.key()] = {it.value().additions, it.value().deletions};
            }
        }
        setDiffStats(add, del, stats);
        countUntrackedStats(add, del, stats);
        refreshWorktreeDiffBodies();
    }, false);
    if (!started) {
        refreshWorktreeDiffBodies();
    }
}

void GitRepositoryService::refreshRefs()
{
    if (!m_available) return;
    m_refsBranchesPending = true;
    m_refsWorktreesPending = true;
    // The list read below is probed again once it lands.
    m_worktreeProbePending = true;
    if (!m_refsRefreshing) {
        m_refsRefreshing = true;
        emit refsRefreshingChanged();
    }
    refreshBranches(false);
}

void GitRepositoryService::settleRefsRead(RefsRead read)
{
    (read == RefsRead::Branches ? m_refsBranchesPending : m_refsWorktreesPending) = false;
    const bool refreshing = m_refsBranchesPending || m_refsWorktreesPending;
    if (refreshing == m_refsRefreshing) return;
    m_refsRefreshing = refreshing;
    emit refsRefreshingChanged();
}

void GitRepositoryService::readWorktrees()
{
    const quint64 epoch = m_refsEpoch;
    const bool started = runGit(
        {QStringLiteral("worktree"), QStringLiteral("list"), QStringLiteral("--porcelain")}, kGitOperationWorktrees,
        [this, epoch](bool ok, const QByteArray &out, const QByteArray &err) {
            // The repository changed since this read began: the list is the
            // old one's, and the switch already ended the refresh.
            if (epoch != m_refsEpoch) return;
            if (!ok) {
                if (m_refsRefreshing) setError(cleanError(err));
                settleRefsRead(RefsRead::Worktrees);
                return;
            }
            QVariantList items = parseWorktrees(out);
            const QString root = QDir::cleanPath(m_repositoryRoot);
            for (QVariant &value : items) {
                QVariantMap item = value.toMap();
                item.insert(QStringLiteral("current"),
                            QString::compare(item.value(QStringLiteral("path")).toString(), root, Qt::CaseInsensitive)
                                == 0);
                value = item;
            }
            m_worktrees.replace(std::move(items));
            settleRefsRead(RefsRead::Worktrees);
            if (m_worktreeProbePending) startWorktreeProbe();
        },
        false);
    if (!started) settleRefsRead(RefsRead::Worktrees);
}

void GitRepositoryService::probeWorktreeChanges()
{
    if (!m_available) return;
    // The list has not landed yet: its read starts the round.
    if (m_worktrees.rowCount() == 0) {
        m_worktreeProbePending = true;
        return;
    }
    startWorktreeProbe();
}

void GitRepositoryService::startWorktreeProbe()
{
    m_worktreeProbePending = false;
    ++m_worktreeProbeGeneration;
    m_worktreeProbeQueue.clear();
    // Results of the worktrees this round checks again stay until their new
    // check lands; the rest (no longer listed, or now past the cap) go. A
    // lone main worktree is the repository itself: its changes already show
    // in the Git pane, so there is nothing to check.
    QVariantMap kept;
    const QVariantList listed = m_worktrees.rowCount() > 1 ? m_worktrees.items() : QVariantList();
    for (const QVariant &value : listed) {
        if (m_worktreeProbeQueue.size() >= kGitWorktreeProbeLimit) break;
        const QVariantMap item = value.toMap();
        const QString path = item.value(QStringLiteral("path")).toString();
        // A bare repository has no worktree to check; a prunable one is gone.
        if (path.isEmpty() || item.value(QStringLiteral("bare")).toBool()
            || item.value(QStringLiteral("prunable")).toBool())
            continue;
        m_worktreeProbeQueue.append(path);
        if (m_worktreeChanges.contains(path)) kept.insert(path, m_worktreeChanges.value(path));
    }
    if (kept != m_worktreeChanges) {
        m_worktreeChanges = kept;
        emit worktreeChangesChanged();
    }
    probeNextWorktree();
}

void GitRepositoryService::probeNextWorktree()
{
    while (!m_worktreeProbeQueue.isEmpty()) {
        const QString path = m_worktreeProbeQueue.takeFirst();
        const quint64 generation = m_worktreeProbeGeneration;
        // Tracked files only: listing untracked ones walks the whole tree.
        // GIT_OPTIONAL_LOCKS=0 (the scheduler's environment) keeps the check
        // from rewriting that worktree's index.
        const bool started = runGitAt(
            path,
            {QStringLiteral("status"), QStringLiteral("--porcelain"), QStringLiteral("-z"),
             QStringLiteral("--untracked-files=no"), QStringLiteral("--ignore-submodules=all")},
            kGitOperationWorktreeStatus,
            [this, path, generation](bool ok, const QByteArray &out, const QByteArray &) {
                // A newer round or a repository switch owns the results now.
                if (generation != m_worktreeProbeGeneration) return;
                const bool known = m_worktreeChanges.contains(path);
                if (ok) {
                    const bool changed = !out.isEmpty();
                    if (!known || m_worktreeChanges.value(path).toBool() != changed) {
                        m_worktreeChanges.insert(path, changed);
                        emit worktreeChangesChanged();
                    }
                } else if (known) {
                    m_worktreeChanges.remove(path);
                    emit worktreeChangesChanged();
                }
                probeNextWorktree();
            },
            false);
        if (started) return;
    }
}

void GitRepositoryService::refreshBranches(bool reportBusy) {
    const quint64 epoch = m_refsEpoch;
    const bool started = runGit(
        {QStringLiteral("for-each-ref"), QStringLiteral("--sort=-committerdate"),
         QStringLiteral("--format=%(refname)%00%(refname:short)%00%(HEAD)%00%(upstream:short)%00%(upstream:track)%00%("
                        "committerdate:iso-strict)%00%(objectname)%00%(*objectname)"),
         QStringLiteral("refs/heads"), QStringLiteral("refs/remotes"), QStringLiteral("refs/tags")},
        kGitOperationBranches,
        [this, reportBusy, epoch](bool ok, const QByteArray &out, const QByteArray &err) {
            // The repository changed since this read began: the refs are the
            // old one's, and the switch already ended the refresh.
            if (epoch != m_refsEpoch) return;
            if (!ok) {
                if (reportBusy || m_refsRefreshing) setError(cleanError(err));
                settleRefsRead(RefsRead::Branches);
                return;
            }
            QVariantList branches = parseBranches(out);
            // Where each ref sat and whether HEAD is on it, before and after:
            // the history rows at both commits change decorations.
            const auto placements = [](const QVariantList &refs) {
                QHash<QString, QPair<QString, bool>> result;
                for (const QVariant &value : refs) {
                    const QVariantMap ref = value.toMap();
                    result.insert(ref.value(QStringLiteral("fullRef")).toString(),
                                  {refCommit(ref), ref.value(QStringLiteral("current")).toBool()});
                }
                return result;
            };
            const auto before = placements(m_branches.items());
            const auto after = placements(branches);
            for (auto it = before.cbegin(); it != before.cend(); ++it) {
                if (after.value(it.key()) != it.value()) {
                    m_historyRefTargets.insert(it.value().first);
                    m_historyRefTargets.insert(after.value(it.key()).first);
                }
            }
            for (auto it = after.cbegin(); it != after.cend(); ++it) {
                if (!before.contains(it.key())) m_historyRefTargets.insert(it.value().first);
            }
            m_historyRefTargets.remove(QString());
            if (m_branches.replace(std::move(branches))) m_historyStale = true;
            settleRefsRead(RefsRead::Branches);
            // Stale until a first page lands: a load deferred behind a queued
            // refresh still runs after that refresh's unchanged branch read.
            if (m_historyStale || m_history.rowCount() == 0) {
                followOnIfUnblocked(true, [this, reportBusy]() { loadHistory(reportBusy); });
            }
        },
        reportBusy);
    if (!started) settleRefsRead(RefsRead::Branches);
    // After the refs, so a branch list never waits for the worktrees.
    readWorktrees();
}

void GitRepositoryService::loadHistory(bool reportBusy) {
    QString ref = m_currentBranch;
    // A detached HEAD reports the porcelain "(detached)" marker instead of a
    // branch name; the commit graph still exists and is reachable via HEAD.
    if (ref == QLatin1String("(detached)")) {
        ref = QStringLiteral("HEAD");
    }
    // Nothing has reported busy for history yet: the branches read that led
    // here settled its own progress before its completion ran.
    if (ref.isEmpty())
        return;
    // A branch without a commit has no history, and `git log <branch>`
    // fails with "bad revision". The bumped request drops a load still in
    // flight (an orphan checkout after a history was shown).
    if (m_headUnborn) {
        ++m_historyRequestGeneration;
        resetHistoryPaging();
        if (m_history.rowCount() > 0)
            m_history.clear();
        return;
    }
    if (!isSafeGitRef(ref)) {
        setError(tr("Invalid history reference."));
        return;
    }
    startHistoryPage(ref, 0, HistoryGraph(), true, reportBusy);
}

void GitRepositoryService::loadMoreHistory()
{
    if (!m_available || m_historyLoading || !m_historyHasMore || !isSafeGitObjectId(m_historyBaseTip)) return;
    startHistoryPage(m_historyBaseTip, m_history.count() - m_historyBaseOffset, m_historyGraph, false, true);
}

void GitRepositoryService::startHistoryPage(const QString &ref, int skip, const HistoryGraph &graph, bool first,
                                            bool reportBusy)
{
    // Every page bumps the request: a first page drops a later page still in
    // flight, whose rows would follow a head that may have moved.
    const quint64 request = ++m_historyRequestGeneration;
    m_historyLoading = true;
    const bool started = runGit(
        {QStringLiteral("log"), ref, QStringLiteral("--topo-order"), QStringLiteral("--date=iso-strict"),
         QStringLiteral("--max-count=%1").arg(kGitHistoryPageSize), QStringLiteral("--skip=%1").arg(skip),
         QStringLiteral("--format=%H%x00%h%x00%P%x00%an%x00%ae%x00%aI%x00%D%x00%s%x1e"),
         // A branch named like a worktree path ("docs") is otherwise an
         // ambiguous argument and the whole history load fails.
         QStringLiteral("--")},
        kGitOperationHistory,
        [this, reportBusy, request, first, graph](bool ok, const QByteArray &, const QByteArray &err) {
            const QString stdoutFile = m_git.takeStdoutFile(GitLane::History);
            auto removeLogFile = [stdoutFile]() {
                if (!stdoutFile.isEmpty()) QFile::remove(stdoutFile);
            };
            const QString operation = kGitOperationHistory;
            if (request != m_historyRequestGeneration) {
                removeLogFile();
                if (reportBusy)
                    m_git.finishLaneTask(GitLane::History, operation, span::core::OperationOutcome::Cancelled);
                return;
            }
            if (!ok) {
                removeLogFile();
                m_historyLoading = false;
                if (reportBusy) {
                    setError(cleanError(err));
                    m_git.finishLaneTask(GitLane::History, operation, span::core::OperationOutcome::Failed,
                                         cleanError(err));
                }
                return;
            }
            if (reportBusy) m_git.setLaneBusy(GitLane::History, true, kGitOperationHistoryParse);
            auto *watcher = new QFutureWatcher<HistoryPage>(this);
            connect(watcher, &QFutureWatcher<HistoryPage>::finished, this,
                    [this, watcher, request, first, reportBusy]() {
                        HistoryPage page = watcher->result();
                        watcher->deleteLater();
                        if (request != m_historyRequestGeneration) {
                            finishHistoryParse(span::core::OperationOutcome::Cancelled, reportBusy);
                            return;
                        }
                        m_historyLoading = false;
                        const int previousCount = m_history.count();
                        if (first) {
                            applyFirstHistoryPage(std::move(page.rows), page.graph);
                        } else {
                            m_historyHasMore = page.rows.size() >= kGitHistoryPageSize;
                            m_historyGraph = page.graph;
                            m_history.append(std::move(page.rows));
                        }
                        if (previousCount == 0 && m_history.count() > 0) emit busyChanged();
                        if (first) {
                            syncSelectionWithHistory(reportBusy);
                            return;
                        }
                        finishHistoryParse(span::core::OperationOutcome::Succeeded, reportBusy);
                    });
            watcher->setFuture(QtConcurrent::run([stdoutFile, graph]() {
                QByteArray data;
                if (!stdoutFile.isEmpty()) {
                    QFile file(stdoutFile);
                    if (file.open(QIODevice::ReadOnly)) data = file.readAll();
                    file.close();
                    QFile::remove(stdoutFile);
                }
                HistoryPage page;
                page.graph = graph;
                page.rows = parseHistoryPage(data, &page.graph);
                return page;
            }));
        },
        reportBusy);
    if (!started) m_historyLoading = false;
}

void GitRepositoryService::applyFirstHistoryPage(QVariantList rows, const HistoryGraph &graph)
{
    const bool more = rows.size() >= kGitHistoryPageSize;
    m_historyStale = false;
    const QSet<QString> refTargets = std::exchange(m_historyRefTargets, {});
    // Rows loaded past the first page stay when the new page only adds
    // commits above the old head.
    if (m_history.count() > rows.size() && !rows.isEmpty()) {
        const QString oldHead = m_history.items().constFirst().toMap().value(QStringLiteral("hash")).toString();
        int oldHeadRow = -1;
        for (int row = 0; row < rows.size(); ++row) {
            if (rows.at(row).toMap().value(QStringLiteral("hash")).toString() == oldHead) {
                oldHeadRow = row;
                break;
            }
        }
        // A moved ref below the new page would leave its old row decorated.
        bool refsBelow = false;
        for (const QString &target : refTargets) {
            if (oldHeadRow >= 0 && m_history.rowOfHash(target) >= rows.size() - oldHeadRow) {
                refsBelow = true;
                break;
            }
        }
        if (oldHeadRow >= 0 && !refsBelow && historyPrefixOnly(rows, oldHeadRow)) {
            m_history.prepend(rows.mid(0, oldHeadRow));
            // Rows the page covers take its decorations.
            for (int row = oldHeadRow; row < rows.size(); ++row) m_history.setItem(row, rows.at(row));
            m_historyBaseOffset += oldHeadRow;
            return;
        }
    }
    if (m_history.count() > rows.size())
        m_history.reset(rows);
    else
        m_history.replace(rows);
    m_historyHasMore = more;
    m_historyGraph = graph;
    m_historyBaseOffset = 0;
    m_historyBaseTip = rows.isEmpty() ? QString() : rows.constFirst().toMap().value(QStringLiteral("hash")).toString();
}

bool GitRepositoryService::historyPrefixOnly(const QVariantList &rows, int oldHeadRow) const
{
    // Rows the page covers are taken from it. The rows kept below join its
    // last row, whose segments carry every lane (and colour) into the next
    // row: when that row is drawn the same, so is everything below.
    const int last = int(rows.size()) - 1;
    return sameGraphRow(rows.at(last).toMap(), m_history.items().at(last - oldHeadRow).toMap());
}

void GitRepositoryService::resetHistoryPaging()
{
    m_historyLoading = false;
    m_historyHasMore = false;
    m_historyBaseTip.clear();
    m_historyBaseOffset = 0;
    m_historyGraph = HistoryGraph();
    m_historyStale = false;
    m_historyRefTargets.clear();
}

void GitRepositoryService::syncSelectionWithHistory(bool reportBusy)
{
    const int selectedRow = m_history.rowOfHash(m_selectedCommit);
    if (selectedRow >= 0) {
        QVariantMap selectedHistoryItem = m_history.itemAt(selectedRow);
        if (m_selectedCommitDetails.contains(QStringLiteral("body"))) {
            selectedHistoryItem.insert(QStringLiteral("body"),
                                       m_selectedCommitDetails.value(QStringLiteral("body")));
        }
        if (selectedHistoryItem != m_selectedCommitDetails) {
            m_selectedCommitDetails = selectedHistoryItem;
            emit selectedCommitChanged();
        }
        finishHistoryParse(span::core::OperationOutcome::Succeeded, reportBusy);
        return;
    }

    if (!m_selectedCommit.isEmpty()) {
        m_selectedCommit.clear();
        m_selectedCommitDetails.clear();
        m_selectedParent.clear();
        resetDiffBodies(DiffScope::Commit);
        m_commitDiffFiles.clear();
        emit selectedCommitChanged();
    }
    const bool refreshQueued = m_git.refreshQueued();
    if (!m_history.items().isEmpty() && !refreshQueued) {
        finishHistoryParse(span::core::OperationOutcome::Succeeded, reportBusy);
        selectCommit(m_history.items().first().toMap().value(QStringLiteral("hash")).toString(), reportBusy);
        return;
    }
    if (refreshQueued)
        scheduleRefresh(true);
    finishHistoryParse(span::core::OperationOutcome::Succeeded, reportBusy);
}

void GitRepositoryService::finishHistoryParse(span::core::OperationOutcome outcome, bool reportBusy)
{
    if (reportBusy) m_git.finishLaneTask(GitLane::History, kGitOperationHistoryParse, outcome);
}

void GitRepositoryService::setError(const QString &message) {
    const QString clean = message.trimmed();
    if (m_errorMessage == clean) return;
    m_errorMessage = clean;
    emit errorMessageChanged();
}

void GitRepositoryService::clearRepository() {
    m_pollTimer.stop();
    m_refreshCoalesce.stop();
    ++m_historyRequestGeneration;
    resetHistoryPaging();
    m_commitPreviews.clear();
    m_untrackedFiles.reset();
    m_pendingIncludeBranches = false;
    // Reads still running belong to the old repository.
    ++m_refsEpoch;
    m_refsBranchesPending = false;
    m_refsWorktreesPending = false;
    if (m_refsRefreshing) {
        m_refsRefreshing = false;
        emit refsRefreshingChanged();
    }
    // Probe checks still running belong to the old repository's worktrees.
    ++m_worktreeProbeGeneration;
    m_worktreeProbeQueue.clear();
    m_worktreeProbePending = false;
    if (!m_worktreeChanges.isEmpty()) {
        m_worktreeChanges.clear();
        emit worktreeChangesChanged();
    }
    m_git.reset();
    m_lastHeadIdentity.clear();
    if (!m_watcher.files().isEmpty()) m_watcher.removePaths(m_watcher.files());
    if (!m_watcher.directories().isEmpty()) m_watcher.removePaths(m_watcher.directories());
    if (!m_repositoryAppearWatcher.directories().isEmpty())
        m_repositoryAppearWatcher.removePaths(m_repositoryAppearWatcher.directories());
    m_workTreeWatcher.stop();
    m_workTreeDirty = true;
    m_repositoryRoot.clear(); m_gitDirectory.clear(); m_gitCommonDirectory.clear();
    m_currentBranch.clear();
    m_selectedCommit.clear(); m_selectedCommitDetails.clear();
    m_selectedParent.clear();
    // The body lane's process was killed with its completion; free the pump.
    ++m_diffBodyRequest;
    m_diffBodyInFlight = false;
    resetDiffBodies(DiffScope::Changes);
    resetDiffBodies(DiffScope::Commit);
    m_available = false; m_statuses.clear(); m_headUnborn = false;
    m_branches.clear();
    m_worktrees.clear();
    m_history.clear();
    m_commitChanges.clear();
    m_commitDiffFiles.clear();
    setDiffStats(0, 0);
    emit repositoryChanged(); emit selectedCommitChanged();
    emit statusMapChanged();
}

QString GitRepositoryService::headIdentity() const {
    if (m_gitDirectory.isEmpty())
        return {};
    QFile file(QDir(m_gitDirectory).filePath(QStringLiteral("HEAD")));
    if (!file.open(QIODevice::ReadOnly))
        return {};
    return QString::fromUtf8(file.readAll());
}

bool GitRepositoryService::watcherImpliesBranchRefresh(const QString &path) const {
    const QString normalized = QDir::fromNativeSeparators(path);
    if (normalized.endsWith(QLatin1String("/HEAD")))
        return true;
    if (normalized.contains(QLatin1String("/refs/")))
        return true;
    const QString gitDir = QDir::fromNativeSeparators(m_gitDirectory);
    const QString commonDir = QDir::fromNativeSeparators(m_gitCommonDirectory);
    if (!gitDir.isEmpty() && (normalized == gitDir || (!commonDir.isEmpty() && normalized == commonDir)))
        return headIdentity() != m_lastHeadIdentity;
    return false;
}

void GitRepositoryService::scheduleRefresh(bool includeBranches) {
    // A background project skips watcher and poll refreshes; showing it
    // again refreshes in full.
    if (!m_available || !m_foreground) return;
    m_pendingIncludeBranches = m_pendingIncludeBranches || includeBranches;
    if (!m_refreshCoalesce.isActive())
        m_refreshCoalesce.start();
}

void GitRepositoryService::flushPendingRefresh() {
    if (!m_available || m_git.laneRunning(GitLane::Status)) {
        if (m_available && m_git.laneRunning(GitLane::Status))
            m_refreshCoalesce.start();
        return;
    }
    const bool includeBranches = m_pendingIncludeBranches;
    m_pendingIncludeBranches = false;
    refreshStatus(includeBranches, false);
}

void GitRepositoryService::configureWatcher() {
    if (m_gitDirectory.isEmpty()) return;
    const QString gitDir = m_gitDirectory;
    QStringList files, dirs;
    for (const QString &path : {QDir(gitDir).filePath(QStringLiteral("HEAD")),
                                QDir(gitDir).filePath(QStringLiteral("index"))})
        if (QFileInfo::exists(path) && !m_watcher.files().contains(path)) files << path;
    const QString commonDir = m_gitCommonDirectory.isEmpty() ? gitDir : m_gitCommonDirectory;
    for (const QString &path : {gitDir, commonDir,
                                QDir(commonDir).filePath(QStringLiteral("refs/heads")),
                                QDir(commonDir).filePath(QStringLiteral("refs/remotes"))})
        if (QFileInfo(path).isDir() && !m_watcher.directories().contains(path)) dirs << path;
    if (!files.isEmpty()) m_watcher.addPaths(files);
    if (!dirs.isEmpty()) m_watcher.addPaths(dirs);
}
