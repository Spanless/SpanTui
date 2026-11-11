#include "documents/code_preview_line_model.h"

#include <algorithm>

namespace {

int expandedColumns(const QString &text)
{
    int columns = 0;
    for (const QChar ch : text) columns += ch == QLatin1Char('\t') ? 4 - columns % 4 : 1;
    return columns;
}

} // namespace

CodePreviewLineModel::CodePreviewLineModel(QObject *parent) : QAbstractListModel(parent) {}

int CodePreviewLineModel::rowCount(const QModelIndex &parent) const
{
    return parent.isValid() ? 0 : count();
}

QVariant CodePreviewLineModel::data(const QModelIndex &index, int role) const
{
    if (!index.isValid() || index.row() < 0 || index.row() >= m_lines.size()) return {};
    const Line &line = m_lines.at(index.row());
    switch (role) {
    case Qt::DisplayRole:
    case TextRole: return line.text;
    case GitStatusRole: return line.gitStatus;
    case DeletionBeforeRole: return line.deletionBefore;
    default: return {};
    }
}

QHash<int, QByteArray> CodePreviewLineModel::roleNames() const
{
    return {
        {TextRole, "text"},
        {GitStatusRole, "gitStatus"},
        {DeletionBeforeRole, "deletionBefore"},
    };
}

QString CodePreviewLineModel::lineText(int oneBasedLine) const
{
    const int row = oneBasedLine - 1;
    return row >= 0 && row < m_lines.size() ? m_lines.at(row).text : QString();
}

void CodePreviewLineModel::setText(const QString &text)
{
    const int previousCount = count();
    beginResetModel();
    m_lines.clear();
    int columns = 0;
    qsizetype start = 0;
    while (true) {
        const qsizetype end = text.indexOf(QLatin1Char('\n'), start);
        Line line;
        line.text = text.mid(start, end < 0 ? -1 : end - start);
        columns = std::max(columns, expandedColumns(line.text));
        m_lines.push_back(std::move(line));
        if (end < 0) break;
        start = end + 1;
    }
    endResetModel();
    if (count() != previousCount) emit countChanged();
    setMaxColumns(columns);
}

void CodePreviewLineModel::clear()
{
    if (m_lines.isEmpty()) return;
    beginResetModel();
    m_lines.clear();
    endResetModel();
    emit countChanged();
    setMaxColumns(0);
}

bool CodePreviewLineModel::setGitLines(const QHash<int, QString> &statuses, const QSet<int> &deletedBefore)
{
    static const QList<int> roles{GitStatusRole, DeletionBeforeRole};
    bool anyChanged = false;
    int runStart = -1;
    const auto flushRun = [&](int endRow) {
        if (runStart >= 0) emit dataChanged(index(runStart), index(endRow), roles);
        runStart = -1;
    };
    for (int row = 0; row < m_lines.size(); ++row) {
        Line &line = m_lines[row];
        const QString status = statuses.value(row + 1);
        const bool deletion = deletedBefore.contains(row + 1);
        if (line.gitStatus == status && line.deletionBefore == deletion) {
            flushRun(row - 1);
            continue;
        }
        line.gitStatus = status;
        line.deletionBefore = deletion;
        anyChanged = true;
        if (runStart < 0) runStart = row;
    }
    flushRun(int(m_lines.size()) - 1);
    return anyChanged;
}

void CodePreviewLineModel::setMaxColumns(int columns)
{
    if (m_maxColumns == columns) return;
    m_maxColumns = columns;
    emit maxColumnsChanged();
}
