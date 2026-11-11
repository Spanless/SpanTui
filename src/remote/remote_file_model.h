#pragma once

#include "remote/remote_file_listing.h"
#include "remote/ssh_command.h"

#include <QAbstractListModel>
#include <QHash>
#include <QUrl>
#include <QVariantList>

namespace span::remote {

class RemoteShellChannel;
class RemoteTransferQueue;

// One remote directory, listed through the shell channel. Owns the channel
// and the transfer queue. A listing for a directory already left is dropped.
// Not final: QML type registration derives from it (application/runtime/qml_types.h).
class RemoteFileModel : public QAbstractListModel {
    Q_OBJECT
    Q_PROPERTY(QString currentPath READ currentPath NOTIFY currentPathChanged FINAL)
    Q_PROPERTY(bool loading READ loading NOTIFY loadingChanged FINAL)
    Q_PROPERTY(QString connectionState READ connectionState NOTIFY connectionStateChanged FINAL)
    Q_PROPERTY(QString errorMessage READ errorMessage NOTIFY errorMessageChanged FINAL)
    Q_PROPERTY(int count READ count NOTIFY countChanged FINAL)
    Q_PROPERTY(QString sortKey READ sortKey NOTIFY sortChanged FINAL)
    Q_PROPERTY(bool sortAscending READ sortAscending NOTIFY sortChanged FINAL)
    Q_PROPERTY(bool showHidden READ showHidden WRITE setShowHidden NOTIFY showHiddenChanged FINAL)

public:
    enum Role {
        NameRole = Qt::UserRole + 1,
        PathRole,
        IsDirRole,
        IsLinkRole,
        SizeTextRole,
        ModifiedTextRole,
        PermissionsRole,
        OwnerRole,
    };

    explicit RemoteFileModel(QObject *parent = nullptr);
    ~RemoteFileModel() override;

    int rowCount(const QModelIndex &parent = {}) const override;
    QVariant data(const QModelIndex &index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

    // Empty askpass environment: the prompt bridge is down. A new endpoint
    // or `keyChanged` drops the channel and listing and, if the browser was
    // in use, reconnects at the new home.
    void configure(const SshEndpoint &endpoint, const QHash<QString, QString> &environment,
                   bool keyChanged = false);
    RemoteShellChannel *channel() const { return m_channel; }
    RemoteTransferQueue *transfers() const { return m_transfers; }

    QString currentPath() const { return m_currentPath; }
    bool loading() const { return m_loading; }
    QString connectionState() const;
    QString errorMessage() const { return m_errorMessage; }
    int count() const { return int(m_rows.size()); }
    QString sortKey() const { return m_sortKey; }
    bool sortAscending() const { return m_sortAscending; }
    bool showHidden() const { return m_showHidden; }
    void setShowHidden(bool show);

    // Connects on first use and lists the home directory.
    Q_INVOKABLE void activate();
    Q_INVOKABLE void openPath(const QString &path);
    Q_INVOKABLE void openRow(int row);
    Q_INVOKABLE void goUp();
    Q_INVOKABLE void goHome();
    Q_INVOKABLE void refresh();
    Q_INVOKABLE void reconnect();
    Q_INVOKABLE void sortBy(const QString &key);
    Q_INVOKABLE void createFolder(const QString &name);
    Q_INVOKABLE void createFile(const QString &name);
    Q_INVOKABLE void rename(const QString &path, const QString &newName);
    Q_INVOKABLE void remove(const QStringList &paths);
    Q_INVOKABLE void upload(const QVariantList &localUrls);
    Q_INVOKABLE void download(const QStringList &remotePaths, const QUrl &localFolder);
    Q_INVOKABLE void copyPathToClipboard(const QString &path) const;
    Q_INVOKABLE void clearError();

signals:
    void currentPathChanged();
    void loadingChanged();
    void connectionStateChanged();
    void errorMessageChanged();
    void countChanged();
    void sortChanged();
    void showHiddenChanged();
    // Success notice for the message center ("Uploaded to …").
    void operationCompleted(const QString &message);

private:
    enum class RequestKind {
        Home,
        List,
        Mutation,
    };
    struct Request {
        RequestKind kind = RequestKind::List;
        quint64 generation = 0;
        QString path;
        QString notice;
    };

    void list(const QString &path);
    void submitMutation(const QString &script, const QString &target, const QString &notice);
    void handleReply(quint64 id, int exitCode, const QByteArray &payload);
    void handleFailure(quint64 id, const QString &message);
    void applyListing(const RemoteListing &listing);
    void rebuildRows();
    void setLoading(bool loading);
    void setError(const QString &message);

    // Identifies the endpoint the listing below belongs to.
    QStringList m_shellArguments;
    bool m_promptBridge = false;
    RemoteShellChannel *m_channel = nullptr;
    RemoteTransferQueue *m_transfers = nullptr;
    QHash<quint64, Request> m_requests;
    QVector<RemoteFileEntry> m_entries;
    QVector<RemoteFileEntry> m_rows;
    QString m_homePath;
    QString m_currentPath;
    QString m_pendingPath;
    quint64 m_listGeneration = 0;
    bool m_activated = false;
    bool m_loading = false;
    QString m_errorMessage;
    QString m_sortKey = QStringLiteral("name");
    bool m_sortAscending = true;
    bool m_showHidden = false;
};

} // namespace span::remote
