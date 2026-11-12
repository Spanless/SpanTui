#pragma once

#include "core/operation_outcome.h"
#include "git/git_diff_store.h"
#include "git/git_lane_scheduler.h"
#include "platform/windows/directory_change_watcher.h"

#include <QAbstractListModel>
#include <QFileSystemWatcher>
#include <QHash>
#include <QPair>
#include <QSet>
#include <QSharedPointer>
#include <QTimer>
#include <QVariantList>
#include <QVariantMap>

#include <functional>

// Not final, like GitRepositoryService: QML type registration derives from
// both (application/runtime/qml_types.h).
class GitItemModel : public QAbstractListModel {
    Q_OBJECT
    Q_PROPERTY(int count READ count NOTIFY countChanged FINAL)
public:
    enum Role { ItemRole = Qt::UserRole + 1 };

    explicit GitItemModel(QObject *parent = nullptr);
    int rowCount(const QModelIndex &parent = {}) const override;
    int count() const;
    QVariant data(const QModelIndex &index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

    const QVariantList &items() const;
    Q_INVOKABLE QVariantMap itemAt(int row) const;
    bool replace(QVariantList items);
    void append(QVariantList items);
    void prepend(QVariantList items);
    // One model reset, for a wholesale swap replace() would diff row by row.
    void reset(QVariantList items);
    void setItem(int row, const QVariant &item);
    void clear();
    int rowOfHash(const QString &hash) const;

private:
    static QString identityKey(const QVariantMap &item);
    static bool uniqueIdentities(const QVariantList &items,
                                 QStringList *orderedIds,
                                 QSet<QString> *idSet);
    void invalidateHashRows();

    QVariantList m_items;
    // Built on the first lookup after a change; append extends it in place.
    mutable QHash<QString, int> m_hashRows;
    mutable bool m_hashRowsValid = false;
signals:
    void countChanged();
};

class GitRepositoryService : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool available READ available NOTIFY repositoryChanged FINAL)
    Q_PROPERTY(QString projectPath READ projectPath NOTIFY projectPathChanged FINAL)
    Q_PROPERTY(QString repositoryRoot READ repositoryRoot NOTIFY repositoryChanged FINAL)
    Q_PROPERTY(QString currentBranch READ currentBranch NOTIFY repositoryChanged FINAL)
    Q_PROPERTY(bool busy READ busy NOTIFY busyChanged FINAL)
    Q_PROPERTY(bool historyViewLoading READ historyViewLoading NOTIFY busyChanged FINAL)
    Q_PROPERTY(bool commitViewLoading READ commitViewLoading NOTIFY busyChanged FINAL)
    Q_PROPERTY(bool changesViewLoading READ changesViewLoading NOTIFY busyChanged FINAL)
    Q_PROPERTY(QString errorMessage READ errorMessage NOTIFY errorMessageChanged FINAL)
    Q_PROPERTY(QString selectedCommit READ selectedCommit NOTIFY selectedCommitChanged FINAL)
    Q_PROPERTY(QVariantMap selectedCommitDetails READ selectedCommitDetails NOTIFY selectedCommitChanged FINAL)
    Q_PROPERTY(GitItemModel *branches READ branches CONSTANT FINAL)
    // `git worktree list`, main worktree first; see parseWorktrees.
    Q_PROPERTY(GitItemModel *worktrees READ worktrees CONSTANT FINAL)
    // A refreshRefs() read of branches, tags and worktrees is in flight.
    Q_PROPERTY(bool refsRefreshing READ refsRefreshing NOTIFY refsRefreshingChanged FINAL)
    Q_PROPERTY(GitItemModel *history READ history CONSTANT FINAL)
    Q_PROPERTY(GitItemModel *commitChanges READ commitChanges CONSTANT FINAL)
    Q_PROPERTY(GitItemModel *commitDiffFiles READ commitDiffFiles CONSTANT FINAL)
    Q_PROPERTY(int diffAdditions READ diffAdditions NOTIFY diffStatsChanged FINAL)
    Q_PROPERTY(int diffDeletions READ diffDeletions NOTIFY diffStatsChanged FINAL)
    // Worktree path (as `worktrees` lists it) -> true when it has staged or
    // unstaged changes to tracked files, from the last probeWorktreeChanges()
    // round; a path without an entry is unknown (not probed yet, past the
    // cap, or its check failed).
    Q_PROPERTY(QVariantMap worktreeChanges READ worktreeChanges NOTIFY worktreeChangesChanged FINAL)

public:
    explicit GitRepositoryService(QObject *parent = nullptr);
    ~GitRepositoryService() override;

    bool available() const;
    QString projectPath() const;
    QString repositoryRoot() const;
    QString currentBranch() const;
    bool busy() const;
    bool historyViewLoading() const;
    bool commitViewLoading() const;
    bool changesViewLoading() const;
    QString errorMessage() const;
    QString selectedCommit() const;
    QVariantMap selectedCommitDetails() const;
    GitItemModel *branches();
    GitItemModel *worktrees();
    bool refsRefreshing() const;
    GitItemModel *history();
    GitItemModel *commitChanges();
    GitItemModel *commitDiffFiles();
    int diffAdditions() const;
    int diffDeletions() const;
    QVariantMap worktreeChanges() const;
    int pollIntervalMs() const;
    void setIdle(bool idle);
    // In the background (no tile shows the project) it neither polls nor
    // follows its watchers; returning to the foreground refreshes at once.
    void setForeground(bool foreground);
    bool foreground() const;
    bool polling() const;
    // Until the next setIdle().
    void setPollIntervalForTesting(int milliseconds);

    struct GitNumstatEntry {
        int additions = 0;
        int deletions = 0;
        bool binary = false;
    };

    // Graph lanes and colours after the last parsed commit; a later page
    // continues from it.
    struct HistoryGraph {
        QStringList lanes;
        QHash<QString, int> lineColor;
        int colorCursor = 0;
    };

    Q_INVOKABLE void setProjectPath(const QString &path);
    Q_INVOKABLE void refresh();
    // Re-reads branches, tags and worktrees (the branch popup's refresh
    // button); refsRefreshing holds until both reads have landed. The
    // worktree list it reads is then probed again (probeWorktreeChanges).
    Q_INVOKABLE void refreshRefs();
    // Starts a silent round of `status --untracked-files=no` checks, one
    // worktree at a time on the Probe lane, for at most
    // kGitWorktreeProbeLimit listed worktrees (bare and missing ones are
    // skipped; a repository without linked worktrees has none to check);
    // results land in worktreeChanges as each check ends. Returns at once. A new round, or a repository switch, drops the older round's
    // pending and running checks. Before the worktree list has loaded, the
    // round starts when it lands.
    Q_INVOKABLE void probeWorktreeChanges();
    Q_INVOKABLE void loadMoreHistory();
    bool historyHasMore() const;
    Q_INVOKABLE void clearError();
    Q_INVOKABLE void selectHistoryCommit(const QString &hash);
    QVariantMap statusMap() const;
    Q_INVOKABLE void requestBaseContent(const QString &absolutePath);
    // `scope`: "changes" (worktree) or "commit" (selected commit); only rows
    // of that list are served. Bodies land in a byte-bounded cache and are
    // announced through diffLinesChanged.
    Q_INVOKABLE void requestDiffLines(const QString &scope, const QStringList &paths);
    // {lines, totalLines, truncated, tooLarge, error, binary}, or null when
    // the body is not cached (never fetched, or evicted).
    Q_INVOKABLE QVariant diffLines(const QString &scope, const QString &path);
    // Cached, else empty: requestCommitPreview fetches on its own lane (newer
    // requests supersede) and answers through commitPreviewReady.
    Q_INVOKABLE QVariantMap commitPreview(const QString &hash) const;
    Q_INVOKABLE void requestCommitPreview(const QString &hash);

    static QVariantMap parseStatus(const QByteArray &output, QVariantList *staged,
                                   QVariantList *working, QHash<QString, QString> *statuses);
    static QVariantList parseBranches(const QByteArray &output);
    // Output of `worktree list --porcelain`; `current` is left false.
    static QVariantList parseWorktrees(const QByteArray &output);
    // One `git log` page; `graph` carries the lanes in from the previous
    // page and out to the next one.
    static QVariantList parseHistoryPage(const QByteArray &output, HistoryGraph *graph);
    static QVariantList parseNameStatus(const QByteArray &output);
    static QHash<QString, GitNumstatEntry> parseNumstat(const QByteArray &output);
    static QList<GitDiffFile> parseUnifiedDiffFiles(const QByteArray &output);
    // Output of `show --shortstat --format=%B%x00`: message, NUL, shortstat.
    static QVariantMap parseCommitPreview(const QByteArray &output);

signals:
    void repositoryChanged();
    void projectPathChanged();
    void busyChanged();
    void refsRefreshingChanged();
    // Raw operation names; the application layer maps them to titles.
    void taskStarted(const QString &operation);
    void taskFinished(const QString &operation,
                      span::core::OperationOutcome outcome,
                      const QString &detail);
    void errorMessageChanged();
    void selectedCommitChanged();
    void statusMapChanged();
    void diffStatsChanged();
    void baseContentReady(const QString &absolutePath, const QString &text, bool existsInHead);
    void commitPreviewReady(const QString &hash, const QVariantMap &preview);
    // Bodies of these paths in `scope` changed (arrived, were refetched with
    // new content, or were dropped); an empty list means the whole scope.
    void diffLinesChanged(const QString &scope, const QStringList &paths);
    void worktreeChangesChanged();

private:
    using Completion = GitLaneScheduler::Completion;
    using GitLane = GitLaneScheduler::Lane;

    enum class DiffScope : int { Changes = 0, Commit, Count };
    struct DiffBodyQueue {
        GitDiffStore store;
        QStringList queue;
        // From a batch that overran the output limit; retried one per process.
        QStringList solo;
        QSet<QString> waiting;
        quint64 epoch = 0;
    };

    void refreshDiffStats();
    static bool diffScopeFromName(const QString &name, DiffScope *scope);
    static QString diffScopeName(DiffScope scope);
    GitItemModel &diffScopeModel(DiffScope scope);
    void resetDiffBodies(DiffScope scope);
    void enqueueDiffBodies(DiffScope scope, const QStringList &paths);
    void pumpDiffBodies();
    void startDiffBodyFetch(DiffScope scope, const QStringList &paths);
    void finishDiffBodyFetch(DiffScope scope, const QStringList &paths, quint64 serial,
                             quint64 epoch, bool ok, const QByteArray &out,
                             const QByteArray &err);
    // Parses `out` and reads the paths git leaves out (untracked files, or
    // every file before the first commit) off the GUI thread, then applies
    // the bodies unless the request went stale meanwhile.
    void parseDiffBodies(DiffScope scope, const QStringList &paths, quint64 serial, quint64 epoch,
                         const QByteArray &out);
    void applyDiffBodies(DiffScope scope, const QStringList &paths, const QList<GitDiffFile> &files,
                         const QHash<QString, GitDiffBodyPtr> &diskBodies);
    void failDiffBodies(DiffScope scope, const QStringList &paths, bool tooLarge,
                        const QString &error);
    QList<QPair<QString, QString>> diskBodyPaths(DiffScope scope, const QStringList &paths) const;
    // After a worktree refresh: refetch the most recently viewed bodies and
    // forget the rest, so no stale worktree diff is served.
    void refreshWorktreeDiffBodies();
    void setDiffStats(int additions, int deletions,
                      const QHash<QString, QPair<int, int>> &perFileStats = {});
    // `git diff HEAD --numstat` leaves untracked files out; count their lines
    // off the GUI thread and publish the stats again with them included.
    void countUntrackedStats(int additions, int deletions,
                             QHash<QString, QPair<int, int>> perFileStats);
    bool runGit(const QStringList &arguments, const QString &operation, Completion completion,
                bool reportBusy = true);
    bool runGitAt(const QString &directory, const QStringList &arguments,
                  const QString &operation, Completion completion, bool reportBusy = true);
    void discoverProject();
    void refreshStatus(bool includeBranches, bool reportBusy = true);
    // Reads the refs and, alongside, the worktrees.
    void refreshBranches(bool reportBusy = true);
    void readWorktrees();
    enum class RefsRead { Branches, Worktrees };
    // A branches or worktrees read of the current repository finished (or
    // could not start); the manual refresh ends with the second one.
    void settleRefsRead(RefsRead read);
    // A new probe round over the listed worktrees, and its next check.
    void startWorktreeProbe();
    void probeNextWorktree();
    void loadHistory(bool reportBusy = true);
    // `first`: the newest page of `ref`; otherwise the page after the loaded
    // rows, continuing `graph`.
    void startHistoryPage(const QString &ref, int skip, const HistoryGraph &graph, bool first, bool reportBusy);
    void applyFirstHistoryPage(QVariantList rows, const HistoryGraph &graph);
    // The loaded rows below the new page can stay: the page ends on the
    // same graph row they continue from.
    bool historyPrefixOnly(const QVariantList &rows, int oldHeadRow) const;
    void syncSelectionWithHistory(bool reportBusy);
    void resetHistoryPaging();
    void selectCommit(const QString &hash, bool reportBusy);
    void loadSelectedCommitBody(bool reportBusy = true);
    void loadSelectedCommitFiles(bool reportBusy = true);
    void finishHistoryParse(span::core::OperationOutcome outcome, bool reportBusy);
    void startPolling();
    void watchWorkTree();
    void noteWorkTreeChanges(const QStringList &relativePaths);
    void watchForRepository();
    void setError(const QString &message);
    void clearRepository();
    void configureWatcher();
    void scheduleRefresh(bool includeBranches);
    void flushPendingRefresh();
    void followOnIfUnblocked(bool includeBranches, const std::function<void()> &startNext);
    bool watcherImpliesBranchRefresh(const QString &path) const;
    QString headIdentity() const;
    void rebuildCommitChanges(const QVariantList &staged, const QVariantList &working);
    // Rejects empty paths, absolute escapes and ".." traversal.
    bool resolveWorktreePath(const QString &path, QString *absoluteOut, QString *relativeOut = nullptr) const;
    static bool isSafeGitObjectId(const QString &hash);
    static bool isSafeGitRef(const QString &ref);
    static QString statusName(QChar code);
    static int statusPriority(const QString &status);
    static QString cleanError(const QByteArray &stderrData);
    static QVariantMap changeItem(const QString &path, const QString &oldPath,
                                  const QString &status, bool staged, bool untracked,
                                  bool conflict);

    QString m_projectPath;
    QString m_repositoryRoot;
    QString m_gitDirectory;
    QString m_gitCommonDirectory;
    QString m_currentBranch;
    // The branch has no commit yet (status reports `# branch.oid (initial)`).
    bool m_headUnborn = false;
    QString m_selectedCommit;
    QVariantMap m_selectedCommitDetails;
    QHash<QString, QVariantMap> m_commitPreviews;
    QString m_selectedParent;
    quint64 m_untrackedStatsRequest = 0;
    quint64 m_historyRequestGeneration = 0;
    bool m_historyLoading = false;
    bool m_historyHasMore = false;
    // Rows from m_historyBaseOffset on came from walking this commit; later
    // pages continue that walk, so a moving branch cannot shift them.
    QString m_historyBaseTip;
    int m_historyBaseOffset = 0;
    HistoryGraph m_historyGraph;
    // Refs changed since the last first page; the commits they pointed to
    // before and after (their decorations may have changed).
    bool m_historyStale = false;
    QSet<QString> m_historyRefTargets;
    QString m_errorMessage;
    bool m_available = false;
    bool m_idle = false;
    bool m_foreground = true;
    // Set when a project switch defers discovery past running lanes; the
    // switch must still end in a discovery pass for the new path.
    bool m_pendingDiscover = false;
    GitLaneScheduler m_git;
    QFileSystemWatcher m_watcher;
    QFileSystemWatcher m_repositoryAppearWatcher;
    // The working tree, recursively: a poll runs `git status` only when
    // something outside the git directory changed since the last one (or the
    // tree cannot be watched).
    span::platform::DirectoryChangeWatcher m_workTreeWatcher;
    bool m_workTreeDirty = true;
    struct UntrackedFileCache;
    QSharedPointer<UntrackedFileCache> untrackedFileCache();
    QSharedPointer<UntrackedFileCache> m_untrackedFiles;
    QTimer m_pollTimer;
    QTimer m_refreshCoalesce;
    bool m_pendingIncludeBranches = false;
    QString m_lastHeadIdentity;
    QHash<QString, QString> m_statuses;
    // Bumped by every repository switch; a branches or worktrees read that
    // finishes under another epoch belongs to the old repository and is
    // dropped.
    quint64 m_refsEpoch = 0;
    bool m_refsBranchesPending = false;
    bool m_refsWorktreesPending = false;
    bool m_refsRefreshing = false;
    QVariantMap m_worktreeChanges;
    // Worktrees the current round has yet to check. A check whose
    // generation is no longer current belongs to an older round (or
    // repository) and is dropped.
    QStringList m_worktreeProbeQueue;
    quint64 m_worktreeProbeGeneration = 0;
    // A round was asked for before the worktree list landed.
    bool m_worktreeProbePending = false;
    DiffBodyQueue m_diffBodies[int(DiffScope::Count)];
    // One body fetch runs at a time; the serial ties a completion to the
    // request that is still current (a repository switch bumps it).
    quint64 m_diffBodyRequest = 0;
    bool m_diffBodyInFlight = false;
    int m_diffBodyNextScope = 0;
    GitItemModel m_branches;
    GitItemModel m_worktrees;
    GitItemModel m_history;
    GitItemModel m_commitChanges;
    GitItemModel m_commitDiffFiles;
    int m_diffAdditions = 0;
    int m_diffDeletions = 0;
};
