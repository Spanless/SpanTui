#pragma once

#include "remote/remote_constants.h"

#include <QHash>
#include <QObject>
#include <QPointer>
#include <QString>

class QLocalServer;
class QLocalSocket;

namespace span::remote {

enum class SshPromptKind {
    // The reusable login password.
    Password = 0,
    // Unlocks the local private key; never sent to a server.
    Passphrase,
    HostKey,
    // Any other question, answered as typed.
    Text,
    // A masked one-off answer: one-time passwords, PINs and other server
    // challenges that mention a password. Never filled in or kept.
    Secret,
};

SshPromptKind classifySshPrompt(const QString &prompt);
// True when the asking ssh (the askpass helper's parent) was started by
// `ownerPid` itself, or by an sftp.exe that `ownerPid` started. A ProxyJump
// or ProxyCommand hop inherits the askpass environment but is started by
// that ssh, a level further down. Prompt text cannot tell them apart: it
// names user@host without the port.
bool isLaunchedSsh(quint32 sshPid, quint32 ownerPid);

// True when this process was started by ssh as its askpass helper.
bool isAskpassInvocation(int argc);
// Askpass helper entry: reads the prompt from the UTF-16 command line, asks
// the broker named in the environment and writes the answer to stdout.
// Returns the process exit code (non-zero = refused).
int runAskpassClient();
// One request/response exchange with a broker pipe. Blocking; used by the
// helper entry and by tests. Returns false when the broker refused or failed.
bool exchangeAskpassPrompt(const QString &pipeName, const QString &token,
                           quint32 parentProcessId, const QString &prompt,
                           QString *answer);

// Owner-only pipe. Each ssh launch carries a token naming its host; unknown
// tokens are refused. Answers come from the application via respond().
class SshAskpassBroker final : public QObject {
    Q_OBJECT

public:
    explicit SshAskpassBroker(QObject *parent = nullptr);
    ~SshAskpassBroker() override;

    bool listen(QString *error = nullptr);
    // Why the last listen() failed; empty once listening.
    QString listenError() const;
    // \\.\pipe\... path the helper opens.
    QString pipeName() const;
    // Defaults to this executable.
    void setAskpassProgram(const QString &program);

    QString issueToken(const QString &hostId);
    void revokeToken(const QString &token);
    // Variables to add to an ssh/sftp process launched with `token`.
    QHash<QString, QString> environment(const QString &token) const;

    // `attempt` counts prompts of the same kind from one ssh process, so a
    // saved secret is offered once and a rejected one is asked for again.
    void respond(quint64 requestId, bool accepted, const QString &answer);

signals:
    // `launched`: isLaunchedSsh() for the asking process.
    void promptRequested(quint64 requestId, const QString &hostId,
                         const QString &prompt, int kind, int attempt, bool launched);
    // The helper went away (ssh exited or was killed) before an answer.
    void promptAbandoned(quint64 requestId);

private:
    struct Pending {
        QPointer<QLocalSocket> socket;
    };

    void acceptConnection();
    void readRequest(QLocalSocket *socket);
    void refuse(QLocalSocket *socket);

    QLocalServer *m_server = nullptr;
    QString m_program;
    QString m_listenError;
    QHash<QString, QString> m_hostByToken;
    QHash<QString, int> m_attempts;
    QHash<quint64, Pending> m_pending;
    quint64 m_nextRequestId = 1;
};

} // namespace span::remote
