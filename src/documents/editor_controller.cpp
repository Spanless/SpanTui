#include "documents/editor_controller.h"

#include <QRegularExpression>
#include <QStringList>

#include <algorithm>

namespace {

constexpr int kGitDiffDebounceMs = 180;
// Lines searched ahead for a resync point after a changed line.
constexpr int kDiffLookahead = 64;

} // namespace

EditorController::EditorController(QObject *parent) : QObject(parent)
{
    m_gitDiffTimer.setSingleShot(true);
    m_gitDiffTimer.setInterval(kGitDiffDebounceMs);
    connect(&m_gitDiffTimer, &QTimer::timeout, this, &EditorController::rebuildGitLines);
}

int EditorController::cursorLine() const
{
    return m_cursorLine;
}
int EditorController::cursorColumn() const
{
    return m_cursorColumn;
}
int EditorController::selectionLength() const
{
    return m_selectionLength;
}
int EditorController::matchCount() const
{
    return m_matches.size();
}
bool EditorController::searchValid() const
{
    return m_searchValid;
}
int EditorController::lineCount() const
{
    if (m_text.isEmpty()) return 1;
    return int(m_text.count(QLatin1Char('\n')) + 1);
}

void EditorController::setText(const QString &text)
{
    QString cleanText = text;
    cleanText.remove(QLatin1Char('\r'));
    // A cleared document keeps no rows, so the same text still repopulates.
    if (m_text == cleanText && m_lineModel.count() > 0) return;
    m_text = cleanText;
    m_lineModel.setText(m_text);
    if (!m_searchQuery.isEmpty()) setSearch(m_searchQuery, m_searchCaseSensitive, m_searchWholeWord, m_searchRegex);
    if (m_hasBase) m_gitDiffTimer.start();
    emit documentMetricsChanged();
}

void EditorController::clearDocument()
{
    clearBaseContent();
    m_lineModel.clear();
    if (m_text.isEmpty()) return;
    m_text.clear();
    if (!m_searchQuery.isEmpty()) setSearch(m_searchQuery, m_searchCaseSensitive, m_searchWholeWord, m_searchRegex);
    emit documentMetricsChanged();
}

void EditorController::setCursorInfo(int line, int column, int selectionLength)
{
    line = std::max(1, line);
    column = std::max(1, column);
    selectionLength = std::max(0, selectionLength);
    if (line == m_cursorLine && column == m_cursorColumn && selectionLength == m_selectionLength) return;
    m_cursorLine = line;
    m_cursorColumn = column;
    m_selectionLength = selectionLength;
    emit cursorInfoChanged();
}

void EditorController::setSearch(const QString &query, bool caseSensitive, bool wholeWord, bool regex)
{
    m_searchQuery = query;
    m_searchCaseSensitive = caseSensitive;
    m_searchWholeWord = wholeWord;
    m_searchRegex = regex;
    m_matches.clear();
    m_searchValid = true;
    if (!query.isEmpty()) {
        QRegularExpression expression(regex ? query : QRegularExpression::escape(query));
        QRegularExpression::PatternOptions options = QRegularExpression::UseUnicodePropertiesOption;
        if (!caseSensitive) options |= QRegularExpression::CaseInsensitiveOption;
        expression.setPatternOptions(options);
        if (wholeWord) expression.setPattern(QStringLiteral("\\b(?:%1)\\b").arg(expression.pattern()));
        m_searchValid = expression.isValid();
        if (m_searchValid) {
            auto iterator = expression.globalMatch(m_text);
            int steps = 0;
            while (iterator.hasNext() && m_matches.size() < 10000 && ++steps < 50000) {
                const auto match = iterator.next();
                if (match.capturedLength() > 0)
                    m_matches.push_back({int(match.capturedStart()), int(match.capturedEnd())});
            }
        }
    }
    emit searchChanged();
}

QVariantMap EditorController::nextMatch(int cursorPosition, bool backwards) const
{
    if (m_matches.isEmpty()) return {};
    QPair<int, int> chosen = backwards ? m_matches.last() : m_matches.first();
    if (backwards) {
        for (int i = m_matches.size() - 1; i >= 0; --i) {
            if (m_matches.at(i).first < cursorPosition) {
                chosen = m_matches.at(i);
                break;
            }
        }
    } else {
        for (const auto &match : m_matches) {
            if (cursorPosition < 0 || match.first > cursorPosition) {
                chosen = match;
                break;
            }
        }
    }
    return {{QStringLiteral("start"), chosen.first}, {QStringLiteral("end"), chosen.second}};
}

int EditorController::offsetForLine(int oneBasedLine) const
{
    if (oneBasedLine <= 1) return 0;
    int remaining = oneBasedLine - 1;
    int offset = 0;
    while (remaining > 0) {
        const int next = m_text.indexOf(QLatin1Char('\n'), offset);
        if (next < 0) return m_text.size();
        offset = next + 1;
        --remaining;
    }
    return offset;
}

int EditorController::goToLine(int oneBasedLine) const
{
    return offsetForLine(oneBasedLine);
}

int EditorController::lineForOffset(int offset) const
{
    if (offset <= 0 || m_text.isEmpty()) return 1;
    const int bounded = std::min(offset, int(m_text.size()));
    int line = 1;
    for (int i = 0; i < bounded; ++i) {
        if (m_text.at(i) == QLatin1Char('\n')) {
            ++line;
        }
    }
    return line;
}

QVariantMap EditorController::highlightLines() const
{
    QVariantMap result;
    if (m_matches.isEmpty() || m_text.isEmpty()) return result;

    int matchIdx = 0;
    int currentLine = 1;
    const int totalMatches = m_matches.size();
    const int textLen = m_text.size();

    for (int i = 0; i < textLen && matchIdx < totalMatches; ++i) {
        while (matchIdx < totalMatches && m_matches.at(matchIdx).first <= i) {
            result.insert(QString::number(currentLine), true);
            ++matchIdx;
        }
        if (m_text.at(i) == QLatin1Char('\n')) {
            ++currentLine;
        }
    }
    while (matchIdx < totalMatches) {
        result.insert(QString::number(currentLine), true);
        ++matchIdx;
    }
    return result;
}

void EditorController::setBaseContent(const QString &text, bool existsInHead)
{
    QString cleanText = text;
    cleanText.remove(QLatin1Char('\r'));
    m_hasBase = true;
    m_baseText = cleanText;
    m_existsInHead = existsInHead;
    m_gitDiffTimer.start();
}

void EditorController::clearBaseContent()
{
    m_hasBase = false;
    m_baseText.clear();
    m_existsInHead = false;
    m_gitDiffTimer.stop();
    m_lineModel.setGitLines({}, {});
}

void EditorController::rebuildGitLines()
{
    QHash<int, QString> gitLines;
    QSet<int> deletedBefore;
    const QStringList current = m_text.split('\n');
    if (!m_existsInHead) {
        for (int line = 1; line <= current.size(); ++line) gitLines.insert(line, QStringLiteral("added"));
        m_lineModel.setGitLines(gitLines, deletedBefore);
        return;
    }
    const QStringList base = m_baseText.split('\n');
    int oldIndex = 0;
    int newIndex = 0;
    while (oldIndex < base.size() && newIndex < current.size()) {
        if (base.at(oldIndex) == current.at(newIndex)) {
            ++oldIndex;
            ++newIndex;
            continue;
        }
        int oldMatch = -1;
        int newMatch = -1;
        for (int step = 1; step <= kDiffLookahead && oldIndex + step < base.size(); ++step) {
            if (base.at(oldIndex + step) == current.at(newIndex)) {
                oldMatch = step;
                break;
            }
        }
        for (int step = 1; step <= kDiffLookahead && newIndex + step < current.size(); ++step) {
            if (current.at(newIndex + step) == base.at(oldIndex)) {
                newMatch = step;
                break;
            }
        }
        if (oldMatch > 0 && (newMatch < 0 || oldMatch <= newMatch)) {
            deletedBefore.insert(newIndex + 1);
            oldIndex += oldMatch;
        } else if (newMatch > 0) {
            for (int step = 0; step < newMatch; ++step) gitLines.insert(newIndex + step + 1, QStringLiteral("added"));
            newIndex += newMatch;
        } else {
            gitLines.insert(newIndex + 1, QStringLiteral("modified"));
            ++oldIndex;
            ++newIndex;
        }
    }
    if (newIndex < current.size()) {
        while (newIndex < current.size()) gitLines.insert(++newIndex, QStringLiteral("added"));
    }
    if (oldIndex < base.size()) deletedBefore.insert(std::max(1, std::min(newIndex + 1, int(current.size()))));
    m_lineModel.setGitLines(gitLines, deletedBefore);
}
