#pragma once

#include <QAbstractListModel>
#include <QHash>
#include <QSet>
#include <QString>
#include <QVector>

// Rows of the read-only file preview: one per line of the document, with the
// git hunk state computed against HEAD. Git updates only touch the rows whose
// state changed, so a rebuild with the same result emits nothing.
class CodePreviewLineModel : public QAbstractListModel {
    Q_OBJECT
    Q_PROPERTY(int count READ count NOTIFY countChanged FINAL)
    // Widest line in columns, tabs expanded to 4-column stops.
    Q_PROPERTY(int maxColumns READ maxColumns NOTIFY maxColumnsChanged FINAL)

public:
    enum Role {
        TextRole = Qt::UserRole + 1,
        GitStatusRole,
        DeletionBeforeRole,
    };

    explicit CodePreviewLineModel(QObject *parent = nullptr);

    int rowCount(const QModelIndex &parent = {}) const override;
    QVariant data(const QModelIndex &index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

    int count() const { return int(m_lines.size()); }
    int maxColumns() const { return m_maxColumns; }
    Q_INVOKABLE QString lineText(int oneBasedLine) const;

    // Replaces the rows with the lines of `text` ('\n' separated); git states
    // start empty.
    void setText(const QString &text);
    void clear();
    // Applies per-line git states keyed by one-based line. Returns whether any
    // row changed; only changed rows are reported through dataChanged.
    bool setGitLines(const QHash<int, QString> &statuses, const QSet<int> &deletedBefore);

signals:
    void countChanged();
    void maxColumnsChanged();

private:
    struct Line {
        QString text;
        QString gitStatus;
        bool deletionBefore = false;
    };

    void setMaxColumns(int columns);

    QVector<Line> m_lines;
    int m_maxColumns = 0;
};
