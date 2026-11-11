#pragma once

#include "documents/code_preview_line_model.h"

#include <QObject>
#include <QPair>
#include <QTimer>
#include <QVariantList>
#include <QVector>

class EditorController : public QObject {
    Q_OBJECT
    Q_PROPERTY(int cursorLine READ cursorLine NOTIFY cursorInfoChanged FINAL)
    Q_PROPERTY(int cursorColumn READ cursorColumn NOTIFY cursorInfoChanged FINAL)
    Q_PROPERTY(int selectionLength READ selectionLength NOTIFY cursorInfoChanged FINAL)
    Q_PROPERTY(int matchCount READ matchCount NOTIFY searchChanged FINAL)
    Q_PROPERTY(bool searchValid READ searchValid NOTIFY searchChanged FINAL)
    Q_PROPERTY(int lineCount READ lineCount NOTIFY documentMetricsChanged FINAL)
    // Rows for the file preview, including per-line git hunk states.
    Q_PROPERTY(CodePreviewLineModel *lineModel READ lineModel CONSTANT FINAL)

public:
    explicit EditorController(QObject *parent = nullptr);

    int cursorLine() const;
    int cursorColumn() const;
    int selectionLength() const;
    int matchCount() const;
    bool searchValid() const;
    int lineCount() const;
    CodePreviewLineModel *lineModel() { return &m_lineModel; }

    Q_INVOKABLE void setText(const QString &text);
    // No document shown: empties the text, the rows and the base content.
    Q_INVOKABLE void clearDocument();
    Q_INVOKABLE void setCursorInfo(int line, int column, int selectionLength);
    Q_INVOKABLE void setSearch(const QString &query, bool caseSensitive, bool wholeWord, bool regex);
    Q_INVOKABLE QVariantMap nextMatch(int cursorPosition, bool backwards) const;
    Q_INVOKABLE int goToLine(int oneBasedLine) const;
    Q_INVOKABLE int lineForOffset(int offset) const;
    Q_INVOKABLE QVariantMap highlightLines() const;
    Q_INVOKABLE void setBaseContent(const QString &text, bool existsInHead);
    Q_INVOKABLE void clearBaseContent();

    // Shortens the debounce between an edit and the git-line rebuild.
    void setGitDiffDelayForTesting(int ms) { m_gitDiffTimer.setInterval(ms); }

signals:
    void cursorInfoChanged();
    void searchChanged();
    void documentMetricsChanged();

private:
    void rebuildGitLines();
    int offsetForLine(int oneBasedLine) const;

    QString m_text;
    int m_cursorLine = 1;
    int m_cursorColumn = 1;
    int m_selectionLength = 0;
    QVector<QPair<int, int>> m_matches;
    QString m_searchQuery;
    bool m_searchCaseSensitive = false;
    bool m_searchWholeWord = false;
    bool m_searchRegex = false;
    bool m_searchValid = true;
    // Without base content there is nothing to diff against: rows carry no
    // git state and text changes schedule no rebuild.
    bool m_hasBase = false;
    QString m_baseText;
    bool m_existsInHead = false;
    CodePreviewLineModel m_lineModel;
    QTimer m_gitDiffTimer;
};
