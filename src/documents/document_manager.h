#pragma once

#include "core/operation_outcome.h"

#include <QAbstractListModel>
#include <QFileSystemWatcher>
#include <QHash>
#include <QSet>
#include <QTimer>
#include <QUrl>

#include <functional>

// Not final: QML type registration derives from it (application/runtime/qml_types.h).
class DocumentManager : public QAbstractListModel {
    Q_OBJECT
    Q_PROPERTY(int count READ rowCount NOTIFY countChanged FINAL)
    Q_PROPERTY(int activeIndex READ activeIndex NOTIFY activeDocumentChanged FINAL)
    Q_PROPERTY(QString activeName READ activeName NOTIFY activeDocumentChanged FINAL)
    Q_PROPERTY(QString activeText READ activeText NOTIFY activeTextChanged FINAL)
    Q_PROPERTY(bool activeMarkdown READ activeMarkdown NOTIFY activeDocumentChanged FINAL)
    Q_PROPERTY(QUrl activeBaseUrl READ activeBaseUrl NOTIFY activeDocumentChanged FINAL)
    Q_PROPERTY(bool activeMissing READ activeMissing NOTIFY activeDocumentChanged FINAL)
    Q_PROPERTY(bool loading READ loading NOTIFY loadingChanged FINAL)

public:
    enum Role {
        PathRole = Qt::UserRole + 1,
        NameRole,
        MissingRole
    };

    explicit DocumentManager(QObject *parent = nullptr);
    ~DocumentManager() override;

    int rowCount(const QModelIndex &parent = {}) const override;
    QVariant data(const QModelIndex &index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

    bool initialize(const QString &databasePath);
    int activeIndex() const;
    QString activeName() const;
    QString activeText() const;
    bool activeMarkdown() const;
    QUrl activeBaseUrl() const;
    bool activeMissing() const;
    bool loading() const;
    Q_INVOKABLE bool loadingAt(int index) const;
    Q_INVOKABLE QString textAt(int index) const;
    Q_INVOKABLE QString pathAt(int index) const;
    Q_INVOKABLE int cursorAt(int index) const;
    Q_INVOKABLE int indexForCanonicalPath(const QString &path) const;

    Q_INVOKABLE bool openFile(const QString &path);
    Q_INVOKABLE bool openFileAt(const QString &path, int oneBasedLine, int oneBasedColumn = 1);
    Q_INVOKABLE void activate(int index);
    Q_INVOKABLE void setDocumentCursor(int index, int position);
    Q_INVOKABLE bool closeDocument(int index);
    Q_INVOKABLE void updatePath(const QString &oldPath, const QString &newPath);
    Q_INVOKABLE void markPathDeleted(const QString &path);
    Q_INVOKABLE void refreshPath(const QString &path);
    Q_INVOKABLE void flush();
    void setOwnerScope(const QString &projectPath, const QString &tileId);
    void beginLoading(const QString &title);
    void endLoading();
    void markDocumentLoading(qulonglong id, bool loading);

signals:
    void countChanged();
    void activeDocumentChanged();
    void activeTextChanged();
    void errorOccurred(const QString &message);
    void loadingChanged();
    void progressStarted(const QString &title);
    void progressChanged(qreal fraction, const QString &detail);
    void progressFinished(span::core::OperationOutcome outcome,
                          const QString &detail);
    void documentActivated();

private:
    struct Document {
        qulonglong id = 0;
        QString path;
        QString name;
        QString text;
        int cursor = 0;
        qint64 diskMtime = 0;
        bool markdown = false;
        bool missing = false;
        bool utf8Bom = false;
    };

    struct ReadResult {
        bool ok = false;
        QString text;
        QString error;
        qint64 mtime = 0;
        bool bom = false;
    };

    static void normalizeLineEndings(QString &text);
    static bool isMarkdownPath(const QString &path);
    static void applyRead(Document &document, const ReadResult &read);

    static QString normalizedPath(const QString &path);
    static ReadResult readTextFile(const QString &path);
    int indexForPath(const QString &path) const;
    Document *activeDocument();
    const Document *activeDocument() const;
    void notifyDocument(int index, const QVector<int> &roles = {});
    void schedulePersist();
    void watchPath(const QString &path);
    void handleExternalChange(const QString &path);
    void applyExternalRead(const QString &path, const ReadResult &read);
    // Reads a large file off the GUI thread for document `documentId`,
    // reporting progress meanwhile. `apply` gets the document's row, or -1
    // when it was closed or a newer external change superseded the read.
    void readDocumentAsync(qulonglong documentId, const QString &path, const QString &title,
                           std::function<void(int row, const ReadResult &read)> apply);
    void restore();

    QVector<Document> m_documents;
    qulonglong m_nextDocumentId = 1;
    int m_activeIndex = -1;
    QString m_connectionName;
    QFileSystemWatcher m_watcher;
    QTimer m_persistTimer;
    // Per-path debounce generations so multi-file external changes are not
    // dropped by a single global counter.
    QHash<QString, quint64> m_externalChangeGenerations;
    QHash<QString, QPair<int, int>> m_pendingOpenLocations;
    QString m_ownerProjectPath = QStringLiteral("");
    QString m_ownerTileId = QStringLiteral("");
    QSet<qulonglong> m_loadingDocumentIds;
    int m_progressDepth = 0;
    QString m_progressError;
};
