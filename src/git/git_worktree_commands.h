#pragma once

#include "core/operation_outcome.h"

#include <QObject>
#include <QProcess>
#include <QStringList>

#include <functional>

// The only Git commands that change a repository: `git worktree add` and
// `git worktree remove`, plus the branch read the create dialog needs for
// projects whose GitRepositoryService has not discovered yet (background
// projects never do). One process at a time, apart from the read lanes of
// GitRepositoryService: a write is never superseded or cancelled by a
// repository switch — killing `worktree add` mid-checkout would leave a
// half-made worktree. Processes inherit the application job.
class GitWorktreeCommands final : public QObject {
    Q_OBJECT

public:
    // `error` is Git's own message (English: the shared Git environment).
    using Completion = std::function<void(bool ok, const QString &error)>;
    using BranchCompletion = std::function<void(const QStringList &branches, const QString &current)>;

    explicit GitWorktreeCommands(QObject *parent = nullptr);
    ~GitWorktreeCommands() override;

    bool busy() const;

    // `git worktree add -b <branch> -- <path> <base>` run in `repository`.
    // False (and no completion) when busy or an argument is unsafe.
    bool add(const QString &repository, const QString &branch, const QString &base, const QString &path,
             Completion completion);
    // `git worktree remove [--force] -- <path>` run in `repository`.
    bool remove(const QString &repository, const QString &path, bool force, Completion completion);
    // Local then remote-tracking branches (`origin/HEAD` left out) and the
    // checked-out one; empty lists when the read fails. Reads may run beside
    // a write.
    void listBranches(const QString &repository, BranchCompletion completion);

    // Parses `for-each-ref --format=%(HEAD)%09%(refname)`.
    static QStringList parseBranches(const QByteArray &output, QString *current);

signals:
    void busyChanged();
    // TaskProgressBinding contract (bindProgress).
    void progressStarted(const QString &title);
    void progressChanged(qreal fraction, const QString &detail);
    void progressFinished(span::core::OperationOutcome outcome, const QString &detail);

private:
    bool startWrite(const QString &repository, const QStringList &arguments, const QString &title,
                    Completion completion);

    QProcess *m_write = nullptr;
};
