#pragma once

#include "core/operation_outcome.h"

#include <QAbstractListModel>
#include <QFileInfo>
#include <QFileSystemWatcher>
#include <QHash>
#include <QSet>
#include <QStringList>
#include <QTimer>
#include <QVariantMap>

#include <functional>

// Not final: QML type registration derives from it (application/runtime/qml_types.h).
class FileTreeModel : public QAbstractListModel {
    Q_OBJECT
    Q_PROPERTY(QString errorMessage READ errorMessage NOTIFY errorMessageChanged FINAL)
    Q_PROPERTY(bool canUndo READ canUndo NOTIFY canUndoChanged FINAL)
    Q_PROPERTY(bool busy READ busy NOTIFY busyChanged FINAL)

public:
    enum Role {
        NameRole = Qt::UserRole + 1,
        PathRole,
        DirectoryRole,
        DepthRole,
        ExpandedRole,
        RootRole,
        GitStatusRole,
        AggregateGitStatusRole,
        CategoryRole,
        LoadingRole,
        RevealedRole
    };

    explicit FileTreeModel(QObject *parent = nullptr);
    ~FileTreeModel() override;

    static int mountedChildLimit();

    int rowCount(const QModelIndex &parent = QModelIndex()) const override;
    QVariant data(const QModelIndex &index, int role = Qt::DisplayRole) const override;
    QHash<int, QByteArray> roleNames() const override;

    QString rootPath() const;
    QString errorMessage() const;
    bool canUndo() const;
    bool busy() const;
    int nodeCount() const;
    void setTrashRootForTesting(const QString &path);
    // Moves skip the rename and take the cross-volume copy path, which a
    // test cannot reach on one volume.
    void setCopyMovesForTesting(bool enabled);
    // The product coalesces watcher events for 250 ms.
    void setWatchCoalesceIntervalForTesting(int milliseconds);
    static QString defaultTrashRoot();
    // Undo is per-process state (m_deleteStack is in memory), so entries left
    // by earlier runs can never be restored. The workspace app purges them
    // once at startup instead of letting the directory grow without bound.
    static bool purgeTrash(const QString &trashRoot);

    Q_INVOKABLE void setRootPath(const QString &path);
    Q_INVOKABLE void refresh();
    Q_INVOKABLE void toggleExpanded(int row);
    // Mutations address items by path; an empty or file parent path creates
    // under the project root or beside that file.
    Q_INVOKABLE void createFile(const QString &parentPath, const QString &name);
    Q_INVOKABLE void createFolder(const QString &parentPath, const QString &name);
    Q_INVOKABLE void renamePath(const QString &path, const QString &newName);
    Q_INVOKABLE void deletePath(const QString &path);
    Q_INVOKABLE void undoLastDelete();
    Q_INVOKABLE void clearError();
    Q_INVOKABLE void openPathExternally(const QString &path);
    Q_INVOKABLE int rowForPath(const QString &path) const;
    Q_INVOKABLE bool canRunInProjectTerminal(int row) const;
    Q_INVOKABLE QString projectTerminalCommand(int row) const;
    QStringList independentTerminalArguments(int row) const;
    Q_INVOKABLE bool runInIndependentTerminal(int row);
    Q_INVOKABLE void setGitStatusMap(const QVariantMap &statuses, const QString &repositoryRoot);

signals:
    void errorMessageChanged();
    void canUndoChanged();
    void busyChanged();
    void progressStarted(const QString &title);
    void progressChanged(qreal fraction, const QString &detail);
    void progressFinished(span::core::OperationOutcome outcome,
                          const QString &detail);
    void operationCompleted(const QString &message);
    void pathRenamed(const QString &oldPath, const QString &newPath);
    void pathDeleted(const QString &path);
    void pathRestored(const QString &path);

private:
    void clearRoot();

    struct Node {
        QString name;
        QString path;
        bool isDir = false;
        bool expanded = false;
        bool loaded = false;
        int depth = 0;
        bool isRoot = false;
        bool loading = false;
        quint64 loadToken = 0;
        bool keepMounted = false;
        bool revealed = true;
    };

    struct DeleteOperation {
        QString originalPath;
        QString trashPath;
    };

    static QVector<Node> readChildrenForRoot(const QString &path, int depth,
                                             const QString &rootPath);
    int descendantEnd(int row) const;
    bool pathIsInsideRoot(const QString &path) const;
    void loadChildren(int row);
    void beginAsyncLoad();
    void endAsyncLoad();
    void setError(const QString &message);
    void setBusy(bool value);
    bool validateName(const QString &name) const;
    // Validated destination for a new item, or empty after reporting why.
    QString newChildPath(const QString &parentPath, const QString &name);
    void movePathAsync(const QString &from, const QString &to,
                       std::function<void(bool)> completion);
    void refreshPreservingExpansion(bool reportBusy = true);
    QSet<QString> mountedDirectoryPaths() const;
    void updateDescendantReveal(int parentRow);
    static void computeReveal(QVector<Node> &nodes, int parentRow, int end);
    void applyKeepMounted(int row, int childCount);
    void collapseRemovingDescendants(int row);
    static QVector<Node> buildMountedTree(const QString &rootPath,
                                          const QSet<QString> &expanded,
                                          const QSet<QString> &mounted);
    static bool sameTreeStructure(const QVector<Node> &left,
                                  const QVector<Node> &right);
    void insertNodes(int row, QVector<Node>::const_iterator first, QVector<Node>::const_iterator last);
    void applyRebuiltTree(QVector<Node> nodes);
    void scheduleWatchedRefresh();
    void flushWatchedRefresh();
    void onWatchedPathChanged(const QString &path);
    void syncWatchedDirectories();
    bool shouldIgnoreWatchPath(const QString &path) const;
    QString relativeRepositoryPath(const QString &path) const;
    QString directGitStatus(const QString &path) const;
    QString aggregateGitStatus(const QString &path) const;
    void rebuildAggregateGitStatuses();
    static QString categoryForPath(const QString &path, bool directory);
    static int gitStatusPriority(const QString &status);

    QVector<Node> m_nodes;
    QVector<DeleteOperation> m_deleteStack;
    QString m_rootPath;
    QString m_errorMessage;
    bool m_busy = false;
    bool m_progressFailed = false;
    QString m_progressTitle;
    QString m_trashRootForTesting;
    bool m_copyMovesForTesting = false;
    QString m_repositoryRoot;
    QHash<QString, QString> m_gitStatuses;
    QHash<QString, QString> m_aggregateStatuses;
    quint64 m_loadGeneration = 0;
    quint64 m_watchGeneration = 0;
    int m_pendingAsyncLoads = 0;
    bool m_silentRebuild = false;
    QFileSystemWatcher m_watcher;
    QTimer m_watchCoalesce;
};
