#pragma once

#include "remote/ssh_command.h"

#include <QAbstractListModel>
#include <QHash>
#include <QPointer>
#include <QUrl>
#include <QVariantMap>
#include <QVector>

namespace span::remote {

class RemoteHostContext;
class SshAskpassBroker;

// Saved SSH hosts and the answers to ssh/sftp prompts; see
// docs/services/remote/ssh-remote.md. Keys are used from an owner-only copy:
// OpenSSH on Windows refuses a key other accounts can read.
class RemoteHostRegistry final : public QAbstractListModel {
    Q_OBJECT
    Q_MOC_INCLUDE("remote/remote_host_context.h")
    Q_PROPERTY(int count READ count NOTIFY hostsChanged FINAL)
    Q_PROPERTY(QVariantMap pendingPrompt READ pendingPrompt NOTIFY pendingPromptChanged FINAL)
    // CONSTANT: the broker listens once, before the registry exists.
    Q_PROPERTY(bool promptBridgeAvailable READ promptBridgeAvailable CONSTANT FINAL)
    Q_PROPERTY(QString promptBridgeError READ promptBridgeError CONSTANT FINAL)
    Q_PROPERTY(QString promptBridgePipe READ promptBridgePipe CONSTANT FINAL)

public:
    enum Role {
        IdRole = Qt::UserRole + 1,
        NameRole,
        AddressRole,
        ExpandedRole,
    };

    RemoteHostRegistry(QString databasePath, SshAskpassBroker *broker, QObject *parent = nullptr);
    ~RemoteHostRegistry() override;

    int rowCount(const QModelIndex &parent = {}) const override;
    QVariant data(const QModelIndex &index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

    int count() const { return int(m_hosts.size()); }
    // False when the stored list could not be read: an empty list then says
    // nothing about which hosts exist, so nothing may be pruned against it.
    bool loaded() const { return m_loaded; }
    QStringList hostIds() const;
    bool contains(const QString &hostId) const;
    Q_INVOKABLE QString hostName(const QString &hostId) const;
    // Case-insensitive; "" when none matches.
    Q_INVOKABLE QString hostIdForAddress(const QStringList &addresses) const;
    bool endpointFor(const QString &hostId, SshEndpoint *endpoint) const;

    // Secrets are never returned, only whether one is saved.
    Q_INVOKABLE QVariantMap hostDetails(const QString &hostId) const;
    Q_INVOKABLE QString validationError(const QVariantMap &fields) const;
    // Another address, port or user drops the saved password.
    Q_INVOKABLE bool movesHost(const QVariantMap &fields) const;
    // UNC included: file://server/share/key -> \\server\share\key.
    Q_INVOKABLE QString keyPathFromUrl(const QUrl &url) const;
    // Empty "id" creates. Empty secrets keep the saved value; an empty user
    // means root.
    Q_INVOKABLE QString saveHost(const QVariantMap &fields);
    // Does not end the host's sessions: WorkspaceController::removeRemoteHost
    // does that once this succeeds.
    bool removeHost(const QString &hostId);
    Q_INVOKABLE bool setExpanded(const QString &hostId, bool expanded);
    RemoteHostContext *contextForHost(const QString &hostId);

    QString terminalCommandLine(const QString &hostId) const;
    QVariantMap terminalEnvironment(const QString &hostId);

    QVariantMap pendingPrompt() const;
    bool promptBridgeAvailable() const;
    QString promptBridgeError() const;
    QString promptBridgePipe() const;
    Q_INVOKABLE void answerPrompt(const QString &requestId, bool accepted, const QString &answer, bool remember);

signals:
    void hostsChanged();
    void errorOccurred(const QString &message);
    void pendingPromptChanged();
    void contextCreated(span::remote::RemoteHostContext *context);

private:
    struct Host {
        QString id;
        QString name;
        SshEndpoint endpoint;
        QByteArray password;
        QByteArray keyPassphrase;
        bool expanded = true;
        int sortOrder = 0;
    };
    struct Prompt {
        quint64 requestId = 0;
        QString hostId;
        QString text;
        int kind = 0;
        int attempt = 1;
        // A secret prompt that is provably this host's own login or key:
        // only those are answered from, or saved to, the host's secrets.
        bool forHost = false;
    };

    int indexForId(const QString &hostId) const;
    bool load();
    // Deletes key copies no stored host owns: leftovers of a removal whose
    // file was still in use, or of an interrupted save.
    void removeOrphanKeyCopies() const;
    bool store(const Host &host);
    QString keyCopyPath(const QString &hostId) const;
    bool importKey(const Host &host, const QString &copyPath, QString *error) const;
    // With the imported key copy when it exists.
    SshEndpoint launchEndpoint(const Host &host) const;
    // Revokes the host's askpass token and forgets this run's secrets, host
    // keys and prompts for it; the next launch gets a fresh token.
    void retireSession(const QString &hostId);
    QString tokenFor(const QString &hostId);
    QHash<QString, QString> launchEnvironment(const QString &hostId);
    QString knownSecret(const QString &hostId, int kind) const;
    bool promptIsForHost(const Host &host, const QString &text, int kind, bool launched) const;
    void handlePrompt(quint64 requestId, const QString &hostId, const QString &text, int kind, int attempt,
                      bool launched);
    void dropPrompt(quint64 requestId);
    void resolveQueued(const QString &hostId, int kind, const QString &text, const QString &answer);

    QString m_databasePath;
    QString m_connectionName;
    bool m_loaded = false;
    SshAskpassBroker *m_broker = nullptr;
    QVector<Host> m_hosts;
    QHash<QString, QPointer<RemoteHostContext>> m_contexts;
    QHash<QString, QString> m_tokens;
    // hostId -> prompt kind -> secret typed during this run.
    QHash<QString, QHash<int, QString>> m_sessionSecrets;
    // hostId -> host-key prompts accepted during this run.
    QHash<QString, QStringList> m_acceptedHostKeys;
    QVector<Prompt> m_prompts;
};

} // namespace span::remote
