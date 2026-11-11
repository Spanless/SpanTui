#include "remote/ssh_askpass.h"

#include <QCoreApplication>
#include <QDeadlineTimer>
#include <QDir>
#include <QFile>
#include <QProcess>
#include <QSignalSpy>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTest>

#include <qt_windows.h>
#include <tlhelp32.h>

#include <future>

using namespace span::remote;

class SshAskpassTest final : public QObject {
    Q_OBJECT

private slots:
    void classifiesOpenSshPrompts();
    void launchedSshIsOnlyTheAppsOwnChild();
    void environmentNamesPipeAndToken();
    void brokerRelaysAnswerAndCountsAttempts();
    void brokerRefusesUnknownOrRevokedToken();
    void refusedAnswerFailsTheHelper();
    void detectsAskpassInvocation();
};

namespace {

struct Exchange {
    bool ok = false;
    QString answer;
};

// The helper side blocks on the pipe, so it runs on another thread while the
// broker answers from this thread's event loop.
std::future<Exchange> askAsync(const QString &pipe, const QString &token, quint32 parentPid,
                               const QString &prompt)
{
    return std::async(std::launch::async, [=]() {
        Exchange result;
        result.ok = exchangeAskpassPrompt(pipe, token, parentPid, prompt, &result.answer);
        return result;
    });
}

Exchange finish(std::future<Exchange> &future)
{
    const QDeadlineTimer deadline(10000);
    while (future.wait_for(std::chrono::seconds(0)) != std::future_status::ready
           && !deadline.hasExpired()) {
        QTest::qWait(10);
    }
    // A hung exchange fails the caller's checks instead of blocking here.
    if (future.wait_for(std::chrono::seconds(0)) != std::future_status::ready) {
        return {};
    }
    return future.get();
}

// The pid of `exe` started by `parent`, waiting for it to appear; 0 if none.
quint32 childOf(qint64 parent, const QString &exe)
{
    const QDeadlineTimer deadline(10000);
    while (!deadline.hasExpired()) {
        HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
        PROCESSENTRY32W entry{};
        entry.dwSize = sizeof(entry);
        quint32 found = 0;
        for (BOOL more = Process32FirstW(snapshot, &entry); more && found == 0;
             more = Process32NextW(snapshot, &entry)) {
            if (entry.th32ParentProcessID == quint32(parent)
                && QString::fromWCharArray(entry.szExeFile).compare(exe, Qt::CaseInsensitive) == 0) {
                found = entry.th32ProcessID;
            }
        }
        CloseHandle(snapshot);
        if (found != 0) {
            return found;
        }
        QTest::qWait(20);
    }
    return 0;
}

void terminate(quint32 pid)
{
    if (HANDLE process = OpenProcess(PROCESS_TERMINATE, FALSE, pid)) {
        TerminateProcess(process, 1);
        CloseHandle(process);
    }
}

void killAndWait(std::initializer_list<QProcess *> processes)
{
    for (QProcess *process : processes) {
        process->kill();
        process->waitForFinished();
    }
}

// Long-lived stand-in for an ssh process.
const QStringList kPing{QStringLiteral("ping.exe"), QStringLiteral("-n"), QStringLiteral("30"),
                        QStringLiteral("127.0.0.1")};

} // namespace

void SshAskpassTest::classifiesOpenSshPrompts()
{
    QCOMPARE(classifySshPrompt(QStringLiteral("alice@host's password: ")), SshPromptKind::Password);
    QCOMPARE(classifySshPrompt(QStringLiteral("Enter passphrase for key 'C:\\k': ")),
             SshPromptKind::Passphrase);
    QCOMPARE(classifySshPrompt(QStringLiteral(
                 "The authenticity of host 'h' can't be established.\n"
                 "Are you sure you want to continue connecting (yes/no/[fingerprint])? ")),
             SshPromptKind::HostKey);
    QCOMPARE(classifySshPrompt(QStringLiteral("Verification code: ")), SshPromptKind::Text);
    // Keyboard-interactive: ssh prefixes the server's text with (user@host).
    QCOMPARE(classifySshPrompt(QStringLiteral("(alice@host) Password: ")), SshPromptKind::Password);
    QCOMPARE(classifySshPrompt(QStringLiteral("Password: ")), SshPromptKind::Password);
    // Server text never unlocks a key, and one-time or other password
    // challenges are not the reusable login password.
    QCOMPARE(classifySshPrompt(QStringLiteral("(alice@host) Enter passphrase for key 'C:\\k': ")),
             SshPromptKind::Secret);
    QCOMPARE(classifySshPrompt(QStringLiteral("(alice@host) Continue connecting (yes/no)? ")),
             SshPromptKind::Text);
    QCOMPARE(classifySshPrompt(QStringLiteral("(alice@host) One-time password: ")),
             SshPromptKind::Secret);
    QCOMPARE(classifySshPrompt(QStringLiteral("(alice@host) x's password: ")), SshPromptKind::Secret);
    QCOMPARE(classifySshPrompt(QStringLiteral("Enter alice@host's old password: ")),
             SshPromptKind::Secret);
}

void SshAskpassTest::launchedSshIsOnlyTheAppsOwnChild()
{
    const quint32 self = quint32(QCoreApplication::applicationPid());
    QProcess ssh;
    ssh.start(kPing.constFirst(), kPing.mid(1));
    QVERIFY(ssh.waitForStarted());
    QVERIFY(isLaunchedSsh(quint32(ssh.processId()), self));

    // A ProxyJump/ProxyCommand hop is started by the ssh, not by the app.
    QProcess proxy;
    proxy.start(QStringLiteral("cmd.exe"), QStringList{QStringLiteral("/c")} + kPing);
    QVERIFY(proxy.waitForStarted());
    const quint32 hop = childOf(proxy.processId(), kPing.constFirst());
    QVERIFY(hop != 0);
    QVERIFY(!isLaunchedSsh(hop, self));

    // sftp runs its own ssh child for each transfer.
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString sftpPath = dir.filePath(QStringLiteral("sftp.exe"));
    QVERIFY(QFile::copy(QStandardPaths::findExecutable(QStringLiteral("cmd.exe")), sftpPath));
    QProcess sftp;
    sftp.start(sftpPath, QStringList{QStringLiteral("/c")} + kPing);
    QVERIFY(sftp.waitForStarted());
    const quint32 transferSsh = childOf(sftp.processId(), kPing.constFirst());
    QVERIFY(transferSsh != 0);
    QVERIFY(isLaunchedSsh(transferSsh, self));

    QVERIFY(!isLaunchedSsh(0, self));
    QVERIFY(!isLaunchedSsh(self, self));

    terminate(hop);
    terminate(transferSsh);
    killAndWait({&ssh, &proxy, &sftp});
}

void SshAskpassTest::environmentNamesPipeAndToken()
{
    SshAskpassBroker broker;
    QVERIFY(broker.listen());
    broker.setAskpassProgram(QStringLiteral("C:\\SpanTui\\SpanTui.exe"));
    const QString token = broker.issueToken(QStringLiteral("host-1"));
    QVERIFY(!token.isEmpty());
    QVERIFY(broker.issueToken(QStringLiteral("host-1")) != token);

    const QHash<QString, QString> environment = broker.environment(token);
    QCOMPARE(environment.value(QStringLiteral("SSH_ASKPASS")), QStringLiteral("C:\\SpanTui\\SpanTui.exe"));
    QCOMPARE(environment.value(QStringLiteral("SSH_ASKPASS_REQUIRE")), QStringLiteral("force"));
    QCOMPARE(environment.value(QLatin1String(kAskpassPipeEnv)), broker.pipeName());
    QCOMPARE(environment.value(QLatin1String(kAskpassTokenEnv)), token);
}

void SshAskpassTest::brokerRelaysAnswerAndCountsAttempts()
{
    SshAskpassBroker broker;
    QVERIFY(broker.listen());
    const QString token = broker.issueToken(QStringLiteral("host-1"));
    QSignalSpy requested(&broker, &SshAskpassBroker::promptRequested);
    connect(&broker, &SshAskpassBroker::promptRequested, this,
            [&broker](quint64 id, const QString &, const QString &, int, int attempt) {
                broker.respond(id, true, QStringLiteral("\u79d8\u5bc6 ") + QString::number(attempt));
            });

    // Non-ASCII both ways: the prompt names a user, the answer is the secret.
    const QString prompt = QStringLiteral("\u5f20\u4e09@host's password: ");
    auto first = askAsync(broker.pipeName(), token, 4242, prompt);
    const Exchange one = finish(first);
    QVERIFY(one.ok);
    QCOMPARE(one.answer, QStringLiteral("\u79d8\u5bc6 1"));
    QCOMPARE(requested.size(), 1);
    QCOMPARE(requested.at(0).at(1).toString(), QStringLiteral("host-1"));
    QCOMPARE(requested.at(0).at(2).toString(), prompt);
    QCOMPARE(requested.at(0).at(3).toInt(), int(SshPromptKind::Password));
    // 4242 is no process this test started.
    QVERIFY(!requested.at(0).at(5).toBool());

    // Same ssh process asking the same kind again: its answer was rejected.
    auto second = askAsync(broker.pipeName(), token, 4242, prompt);
    QCOMPARE(finish(second).answer, QStringLiteral("\u79d8\u5bc6 2"));
    // Another ssh process starts its own count.
    auto other = askAsync(broker.pipeName(), token, 5151, prompt);
    QCOMPARE(finish(other).answer, QStringLiteral("\u79d8\u5bc6 1"));
}

void SshAskpassTest::brokerRefusesUnknownOrRevokedToken()
{
    SshAskpassBroker broker;
    QVERIFY(broker.listen());
    QSignalSpy requested(&broker, &SshAskpassBroker::promptRequested);

    auto unknown = askAsync(broker.pipeName(), QStringLiteral("forged"), 1, QStringLiteral("password: "));
    QVERIFY(!finish(unknown).ok);

    const QString token = broker.issueToken(QStringLiteral("host-1"));
    broker.revokeToken(token);
    auto revoked = askAsync(broker.pipeName(), token, 1, QStringLiteral("password: "));
    QVERIFY(!finish(revoked).ok);
    QCOMPARE(requested.size(), 0);
}

void SshAskpassTest::refusedAnswerFailsTheHelper()
{
    SshAskpassBroker broker;
    QVERIFY(broker.listen());
    const QString token = broker.issueToken(QStringLiteral("host-1"));
    connect(&broker, &SshAskpassBroker::promptRequested, this,
            [&broker](quint64 id) { broker.respond(id, false, QStringLiteral("ignored")); });

    auto refused = askAsync(broker.pipeName(), token, 1, QStringLiteral("password: "));
    const Exchange result = finish(refused);
    QVERIFY(!result.ok);
    QVERIFY(result.answer.isEmpty());
}

void SshAskpassTest::detectsAskpassInvocation()
{
    // The test process itself was not started by ssh.
    QVERIFY(qEnvironmentVariableIsEmpty(kAskpassPipeEnv));
    QVERIFY(!isAskpassInvocation(2));
    qputenv(kAskpassPipeEnv, "\\\\.\\pipe\\x");
    qputenv(kAskpassTokenEnv, "t");
    QVERIFY(isAskpassInvocation(2));
    QVERIFY(!isAskpassInvocation(1));
    qunsetenv(kAskpassPipeEnv);
    qunsetenv(kAskpassTokenEnv);
}

QTEST_GUILESS_MAIN(SshAskpassTest)

#include "ssh_askpass_test.moc"
