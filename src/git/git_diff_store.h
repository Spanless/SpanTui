#pragma once

#include <QHash>
#include <QSharedPointer>
#include <QString>
#include <QStringList>
#include <QVariantMap>
#include <QVector>

// Kept compact: large diffs hold many rows; avoid per-row QVariantMaps.
struct GitDiffLine {
    enum Kind : quint8 { Context, Add, Delete };
    QString text;
    int oldLine = 0;
    int newLine = 0;
    Kind kind = Context;
};

// The diff body of one file, capped by kGitDiffMaxLines/kGitDiffMaxColumns.
struct GitDiffBody {
    QVector<GitDiffLine> lines;
    int additions = 0;
    int deletions = 0;
    // Rows in the full diff; above lines.size() when truncated.
    int totalLines = 0;
    bool truncated = false;
    // The diff was over the lane's output limit, so nothing was parsed.
    bool tooLarge = false;
    // Git failed for this file; cached so a view does not refetch in a loop.
    QString error;
    bool binary = false;
    // Detects an unchanged body across worktree refreshes.
    size_t digest = 0;
    // Estimated resident size, charged against the store budget.
    qsizetype bytes = 0;

    void append(GitDiffLine::Kind kind, int oldLine, int newLine, QString text);
    // {lines, totalLines, truncated, tooLarge, error, binary}; rows use the
    // TextLinesView shape {kind, oldLine, newLine, oldText, newText}.
    QVariantMap toVariant() const;
};
using GitDiffBodyPtr = QSharedPointer<const GitDiffBody>;

// One file section of a unified diff: list-row metadata (path, oldPath,
// status, additions, deletions, binary) and the parsed body.
struct GitDiffFile {
    QVariantMap item;
    GitDiffBodyPtr body;
};

// Byte-bounded LRU of diff bodies for one list (worktree or selected
// commit). Views convert what they show into their own copy, so evicting
// never blanks a visible diff; a file expanded again simply refetches.
class GitDiffStore {
public:
    explicit GitDiffStore(qsizetype budgetBytes = qsizetype(32) * 1024 * 1024);

    void clear();
    // Marks the body most recently used.
    GitDiffBodyPtr find(const QString &path);
    bool contains(const QString &path) const;
    // True when the stored content changed (new path or different digest).
    bool insert(const QString &path, GitDiffBodyPtr body);
    void remove(const QString &path);
    // Least recently used first.
    QStringList paths() const;
    qsizetype bytes() const;
    int size() const;

private:
    void touch(const QString &path);
    void evictOverBudget();

    QHash<QString, GitDiffBodyPtr> m_bodies;
    QStringList m_order;
    qsizetype m_bytes = 0;
    qsizetype m_budget = 0;
};
