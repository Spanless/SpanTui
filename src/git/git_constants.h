#pragma once

#include <QLatin1StringView>

// Per-file diff caps, so a generated or minified file cannot flood memory or
// the view: rows past the line cap are dropped (counts still cover the whole
// diff) and text past the column cap is cut.
inline constexpr int kGitDiffMaxLines = 5000;
inline constexpr int kGitDiffMaxColumns = 1000;

inline constexpr int kGitHistoryPageSize = 500;

// Task operation names that the application maps to progress sources and
// titles.
inline constexpr QLatin1StringView kGitOperationRefresh{"refresh"};
inline constexpr QLatin1StringView kGitOperationDiscover{"discover"};
inline constexpr QLatin1StringView kGitOperationHistory{"history"};
inline constexpr QLatin1StringView kGitOperationHistoryParse{"history-parse"};
inline constexpr QLatin1StringView kGitOperationBranches{"branches"};
// `git worktree list`: silent, so no task-progress mapping.
inline constexpr QLatin1StringView kGitOperationWorktrees{"worktrees"};
inline constexpr QLatin1StringView kGitOperationCommitDetails{"commit-details"};
inline constexpr QLatin1StringView kGitOperationCommitFiles{"commit-files"};
inline constexpr QLatin1StringView kGitOperationCommitFilesStats{"commit-files-stats"};
// Silent `status` of one worktree for probeWorktreeChanges(), on the Probe
// lane.
inline constexpr QLatin1StringView kGitOperationWorktreeStatus{"worktree-status"};

// Worktrees one probeWorktreeChanges() round checks, in `worktree list`
// order; the rest stay unknown. Each check is a `git status` of its own.
inline constexpr int kGitWorktreeProbeLimit = 16;
