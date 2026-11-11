#include "git/git_diff_store.h"

#include "git/git_constants.h"

#include <QVariantList>

#include <utility>

namespace {
// Per-row bookkeeping on top of the text payload: the record itself plus the
// QString header.
constexpr qsizetype kLineOverhead = qsizetype(sizeof(GitDiffLine)) + 24;

QString kindName(GitDiffLine::Kind kind)
{
    switch (kind) {
    case GitDiffLine::Add:
        return QStringLiteral("add");
    case GitDiffLine::Delete:
        return QStringLiteral("delete");
    case GitDiffLine::Context:
        break;
    }
    return QStringLiteral("context");
}
} // namespace

void GitDiffBody::append(GitDiffLine::Kind kind, int oldLine, int newLine, QString text)
{
    ++totalLines;
    if (lines.size() >= kGitDiffMaxLines) {
        truncated = true;
        return;
    }
    if (text.size() > kGitDiffMaxColumns) {
        text.truncate(kGitDiffMaxColumns);
        text += QChar(0x2026);
    }
    bytes += kLineOverhead + text.size() * qsizetype(sizeof(QChar));
    lines.push_back(GitDiffLine{std::move(text), oldLine, newLine, kind});
}

QVariantMap GitDiffBody::toVariant() const
{
    const QString oldKey = QStringLiteral("oldText");
    const QString newKey = QStringLiteral("newText");
    QVariantList rows;
    rows.reserve(lines.size());
    for (const GitDiffLine &line : lines) {
        QVariantMap row{{QStringLiteral("kind"), kindName(line.kind)},
                        {QStringLiteral("oldLine"), line.oldLine},
                        {QStringLiteral("newLine"), line.newLine}};
        if (line.kind != GitDiffLine::Add)
            row.insert(oldKey, line.text);
        if (line.kind != GitDiffLine::Delete)
            row.insert(newKey, line.text);
        rows.push_back(std::move(row));
    }
    return {{QStringLiteral("lines"), rows},
            {QStringLiteral("totalLines"), totalLines},
            {QStringLiteral("truncated"), truncated},
            {QStringLiteral("tooLarge"), tooLarge},
            {QStringLiteral("error"), error},
            {QStringLiteral("binary"), binary}};
}

GitDiffStore::GitDiffStore(qsizetype budgetBytes) : m_budget(budgetBytes) {}

void GitDiffStore::clear()
{
    m_bodies.clear();
    m_order.clear();
    m_bytes = 0;
}

GitDiffBodyPtr GitDiffStore::find(const QString &path)
{
    const GitDiffBodyPtr body = m_bodies.value(path);
    if (body)
        touch(path);
    return body;
}

bool GitDiffStore::contains(const QString &path) const
{
    return m_bodies.contains(path);
}

bool GitDiffStore::insert(const QString &path, GitDiffBodyPtr body)
{
    if (!body)
        return false;
    const GitDiffBodyPtr previous = m_bodies.value(path);
    const bool changed = !previous || previous->digest != body->digest
        || previous->tooLarge != body->tooLarge || previous->error != body->error;
    if (previous)
        m_bytes -= previous->bytes;
    m_bytes += body->bytes;
    m_bodies.insert(path, std::move(body));
    touch(path);
    evictOverBudget();
    return changed;
}

void GitDiffStore::remove(const QString &path)
{
    const GitDiffBodyPtr body = m_bodies.take(path);
    if (!body)
        return;
    m_bytes -= body->bytes;
    m_order.removeOne(path);
}

QStringList GitDiffStore::paths() const
{
    return m_order;
}

qsizetype GitDiffStore::bytes() const
{
    return m_bytes;
}

int GitDiffStore::size() const
{
    return int(m_bodies.size());
}

void GitDiffStore::touch(const QString &path)
{
    if (!m_order.isEmpty() && m_order.constLast() == path)
        return;
    m_order.removeOne(path);
    m_order.append(path);
}

void GitDiffStore::evictOverBudget()
{
    // The most recent body always stays, even when it alone is over budget.
    while (m_bytes > m_budget && m_order.size() > 1)
        remove(m_order.constFirst());
}
