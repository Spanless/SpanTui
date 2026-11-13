#pragma once

#include <QList>
#include <QString>

// Worktree relations read straight from the repository's files, without
// starting git: cheap enough for the project registry to call while it adds
// or lists projects. Only the layout git itself writes is understood — a
// main worktree with a `.git` directory, linked worktrees with a `.git` file
// pointing into `<main>/.git/worktrees/<id>`. Anything else (bare
// repositories, separate git dirs, submodules) reads as "no relation".
namespace span::git {

struct LinkedWorktree {
    // Canonical folder, forward slashes.
    QString path;
    // Checked-out branch without `refs/heads/`; empty when detached.
    QString branch;
};

// The main worktree of `folder` when `folder` is a linked worktree, else
// empty. Canonical, forward slashes.
QString mainWorktreeOf(const QString &folder);

// Linked worktrees git lists for the main worktree `mainWorktree` whose
// folders still exist, ordered by path.
QList<LinkedWorktree> linkedWorktreesOf(const QString &mainWorktree);

// What `folder` (a worktree root) has checked out, read from its HEAD file:
// the branch without `refs/heads/`, the first 7 digits of a detached commit,
// or empty when `folder` is not a worktree root.
QString headLabelOf(const QString &folder);

// Whether `folder` is the top of a git worktree (it holds a `.git` directory
// or file). Subfolders of a repository are not.
bool isWorktreeRoot(const QString &folder);

// A new branch name git accepts for `git worktree add -b`: the rules of
// `git check-ref-format --branch` (no `@{-N}` shorthand, no leading `-`).
bool isValidBranchName(const QString &name);

}
