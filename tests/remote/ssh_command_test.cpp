#include "remote/remote_transfer_queue.h"
#include "remote/ssh_command.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSignalSpy>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTest>

#include <utility>

using namespace span::remote;

class SshCommandTest final : public QObject {
    Q_OBJECT

private slots:
    void acceptsOrdinaryEndpoints();
    void rejectsOptionLikeAndMalformedValues_data();
    void rejectsOptionLikeAndMalformedValues();
    void rejectsMissingKeyFile();
    void terminalArgumentsEndOptionsBeforeHost();
    void shellArgumentsRunPosixShellWithoutTty();
    void sftpKeepsAskpassUsableInBatchMode();
    void windowsCommandLineQuotesLikeMsvc();
    void shellQuoteSurvivesSingleQuotes();
    void sftpBracketsIpv6Hosts();
    void sftpQuoteEscapesOnlyQuoteAndBackslash();
    void batchScriptQuotesEveryOperand();
    void transfersKeepGlobNamesAndTheirEndpoint();
};

namespace {

// Git for Windows ships the OpenSSH sftp-server; sftp -D talks to it over a
// pipe, which runs the real batch parser without an SSH server.
QString gitSftpServer()
{
    const QString git = QStandardPaths::findExecutable(QStringLiteral("git"));
    if (git.isEmpty()) {
        return {};
    }
    QDir dir = QFileInfo(git).absoluteDir();
    for (int depth = 0; depth < 3; ++depth) {
        const QString server = dir.filePath(QStringLiteral("usr/lib/ssh/sftp-server.exe"));
        if (QFileInfo(server).isFile()) {
            return server;
        }
        if (!dir.cdUp()) {
            break;
        }
    }
    return {};
}

// The MSYS server sees C:/x as /c/x.
QString msysPath(const QString &path)
{
    const QString absolute = QDir::fromNativeSeparators(QFileInfo(path).absoluteFilePath());
    return QLatin1Char('/') + absolute.at(0).toLower() + absolute.mid(2);
}

// The message of each transferFinished that reported failure.
QStringList failedTransfers(const QSignalSpy &finished)
{
    QStringList failures;
    for (const QList<QVariant> &result : finished) {
        if (!result.at(2).toBool()) failures.append(result.at(3).toString());
    }
    return failures;
}

} // namespace

void SshCommandTest::acceptsOrdinaryEndpoints()
{
    QCOMPARE(checkEndpoint({QStringLiteral("example.com"), 22, QString(), QString()}),
             EndpointError::None);
    QCOMPARE(checkEndpoint({QStringLiteral("192.168.1.10"), 2222, QStringLiteral("root"), QString()}),
             EndpointError::None);
    QCOMPARE(checkEndpoint({QStringLiteral("fe80::1"), 22, QStringLiteral("DOMAIN\\user.name"), QString()}),
             EndpointError::None);
}

void SshCommandTest::rejectsOptionLikeAndMalformedValues_data()
{
    QTest::addColumn<QString>("host");
    QTest::addColumn<int>("port");
    QTest::addColumn<QString>("user");
    QTest::addColumn<int>("expected");

    QTest::newRow("empty host") << QString() << 22 << QString() << int(EndpointError::Host);
    QTest::newRow("option host") << QStringLiteral("-oProxyCommand=calc") << 22 << QString()
                                 << int(EndpointError::Host);
    QTest::newRow("space in host") << QStringLiteral("a b") << 22 << QString()
                                   << int(EndpointError::Host);
    QTest::newRow("percent in host") << QStringLiteral("h%n") << 22 << QString()
                                     << int(EndpointError::Host);
    QTest::newRow("port zero") << QStringLiteral("h") << 0 << QString() << int(EndpointError::Port);
    QTest::newRow("port too large") << QStringLiteral("h") << 65536 << QString()
                                    << int(EndpointError::Port);
    QTest::newRow("option user") << QStringLiteral("h") << 22 << QStringLiteral("-oX=y")
                                 << int(EndpointError::User);
    QTest::newRow("quote in user") << QStringLiteral("h") << 22 << QStringLiteral("a\"b")
                                   << int(EndpointError::User);
}

void SshCommandTest::rejectsOptionLikeAndMalformedValues()
{
    QFETCH(QString, host);
    QFETCH(int, port);
    QFETCH(QString, user);
    QFETCH(int, expected);
    QCOMPARE(int(checkEndpoint({host, port, user, QString()})), expected);
}

void SshCommandTest::rejectsMissingKeyFile()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const SshEndpoint missing{QStringLiteral("h"), 22, QString(), dir.filePath(QStringLiteral("none"))};
    QCOMPARE(checkEndpoint(missing), EndpointError::IdentityFile);
    // A directory is not a key file either.
    const SshEndpoint directory{QStringLiteral("h"), 22, QString(), dir.path()};
    QCOMPARE(checkEndpoint(directory), EndpointError::IdentityFile);
}

void SshCommandTest::terminalArgumentsEndOptionsBeforeHost()
{
    const QStringList arguments = sshTerminalArguments(
        {QStringLiteral("example.com"), 2222, QStringLiteral("alice"), QString()});
    QVERIFY(!arguments.contains(QStringLiteral("-T")));
    QVERIFY(!arguments.contains(QStringLiteral("-i")));
    const qsizetype separator = arguments.indexOf(QStringLiteral("--"));
    QVERIFY(separator > 0);
    QCOMPARE(arguments.mid(separator), (QStringList{QStringLiteral("--"), QStringLiteral("example.com")}));
    QCOMPARE(arguments.at(arguments.indexOf(QStringLiteral("-p")) + 1), QStringLiteral("2222"));
    QCOMPARE(arguments.at(arguments.indexOf(QStringLiteral("-l")) + 1), QStringLiteral("alice"));
}

void SshCommandTest::shellArgumentsRunPosixShellWithoutTty()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString key = dir.filePath(QStringLiteral("id_ed25519"));
    QFile file(key);
    QVERIFY(file.open(QIODevice::WriteOnly));
    file.close();

    const QStringList arguments = sshShellArguments({QStringLiteral("h"), 22, QString(), key});
    QCOMPARE(arguments.constFirst(), QStringLiteral("-T"));
    QCOMPARE(arguments.constLast(), QStringLiteral("exec /bin/sh"));
    QCOMPARE(arguments.at(arguments.size() - 2), QStringLiteral("h"));
    QCOMPARE(arguments.at(arguments.indexOf(QStringLiteral("-i")) + 1), QDir::toNativeSeparators(key));
    QVERIFY(arguments.contains(QStringLiteral("IdentitiesOnly=yes")));
    QVERIFY(!arguments.contains(QStringLiteral("-l")));
}

void SshCommandTest::sftpKeepsAskpassUsableInBatchMode()
{
    const QStringList arguments = sftpBatchArguments(
        {QStringLiteral("h"), 2200, QStringLiteral("bob"), QString()});
    const qsizetype batchMode = arguments.indexOf(QStringLiteral("BatchMode=no"));
    const qsizetype batch = arguments.indexOf(QStringLiteral("-b"));
    QVERIFY(batchMode > 0);
    QVERIFY(batch > batchMode);
    QCOMPARE(arguments.at(batch + 1), QStringLiteral("-"));
    QCOMPARE(arguments.at(arguments.indexOf(QStringLiteral("-P")) + 1), QStringLiteral("2200"));
    QVERIFY(arguments.contains(QStringLiteral("User=bob")));
    QCOMPARE(arguments.mid(arguments.size() - 2), (QStringList{QStringLiteral("--"), QStringLiteral("h")}));
}

void SshCommandTest::windowsCommandLineQuotesLikeMsvc()
{
    QCOMPARE(windowsCommandLine(QStringLiteral("C:\\Windows\\ssh.exe"),
                                {QStringLiteral("-p"), QStringLiteral("22")}),
             QStringLiteral("C:\\Windows\\ssh.exe -p 22"));
    QCOMPARE(windowsCommandLine(QStringLiteral("C:\\Program Files\\ssh.exe"),
                                {QStringLiteral("a b"), QString()}),
             QStringLiteral("\"C:\\Program Files\\ssh.exe\" \"a b\" \"\""));
    // Backslashes before a quote and at the end are doubled.
    QCOMPARE(windowsCommandLine(QStringLiteral("x"), {QStringLiteral("a\\\"b"), QStringLiteral("c d\\")}),
             QStringLiteral("x \"a\\\\\\\"b\" \"c d\\\\\""));
}

void SshCommandTest::shellQuoteSurvivesSingleQuotes()
{
    QCOMPARE(shellQuote(QStringLiteral("/srv/a b")), QStringLiteral("'/srv/a b'"));
    QCOMPARE(shellQuote(QStringLiteral("it's")), QStringLiteral("'it'\\''s'"));
    QCOMPARE(shellQuote(QStringLiteral("$(rm -rf /)")), QStringLiteral("'$(rm -rf /)'"));
}

void SshCommandTest::sftpBracketsIpv6Hosts()
{
    const QStringList arguments = sftpBatchArguments(
        {QStringLiteral("2001:db8::1"), 22, QStringLiteral("bob"), QString()});
    QCOMPARE(arguments.constLast(), QStringLiteral("[2001:db8::1]"));
    // ssh takes the host as is.
    QCOMPARE(sshTerminalArguments({QStringLiteral("2001:db8::1"), 22, QString(), QString()})
                 .constLast(),
             QStringLiteral("2001:db8::1"));
}

void SshCommandTest::sftpQuoteEscapesOnlyQuoteAndBackslash()
{
    QCOMPARE(sftpQuote(QStringLiteral("a\"b\\c")), QStringLiteral("\"a\\\"b\\\\c\""));
    QCOMPARE(sftpQuote(QStringLiteral("*?[x]")), QStringLiteral("\"*?[x]\""));
}

void SshCommandTest::batchScriptQuotesEveryOperand()
{
    const QByteArray upload = RemoteTransferQueue::batchScript(
        RemoteTransferQueue::Direction::Upload,
        {QStringLiteral("C:\\data\\a*.txt"), QStringLiteral("C:\\data\\dir")},
        QStringLiteral("/srv/in box"));
    QCOMPARE(upload, QByteArray("put -r \"C:/data/a*.txt\" \"/srv/in box\"\n"
                                "put -r \"C:/data/dir\" \"/srv/in box\"\n"));
}

void SshCommandTest::transfersKeepGlobNamesAndTheirEndpoint()
{
    // Real sftp.exe against a local sftp-server (Git for Windows) over -D:
    // the batch goes through sftp's own parser and globbing, no network.
    const QString sftp = sftpExecutable();
    const QString server = gitSftpServer();
    if (sftp.isEmpty() || server.isEmpty()) QSKIP("sftp.exe or Git for Windows sftp-server.exe is not available");
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString remote = dir.filePath(QStringLiteral("remote"));
    const QString local = dir.filePath(QStringLiteral("local"));
    QVERIFY(QDir().mkpath(remote));
    QVERIFY(QDir().mkpath(local));
    QVERIFY(QDir().mkpath(dir.filePath(QStringLiteral("download"))));
    QVERIFY(QDir().mkpath(dir.filePath(QStringLiteral("upload"))));
    QFile source(QDir(local).filePath(QStringLiteral("report[1].txt")));
    QVERIFY(source.open(QIODevice::WriteOnly));
    source.write("up");
    source.close();
    QFile decoy(QDir(remote).filePath(QStringLiteral("report1.txt")));
    QVERIFY(decoy.open(QIODevice::WriteOnly));
    decoy.close();
    QFile named(QDir(remote).filePath(QStringLiteral("report[1].txt")));
    QVERIFY(named.open(QIODevice::WriteOnly));
    named.write("down");
    named.close();

    RemoteTransferQueue queue;
    // The MSYS server loads msys-2.0.dll from Git's usr/bin.
    const QString msysBin = QDir::toNativeSeparators(QDir::cleanPath(
        QFileInfo(server).absoluteDir().absoluteFilePath(QStringLiteral("../../bin"))));
    // -D takes a server command line, not a path: quote it so Git under
    // "C:\Program Files" (as on CI) is not split at the space.
    queue.setLaunch(sftp, {QStringLiteral("-b"), QStringLiteral("-"), QStringLiteral("-D"),
                           QLatin1Char('"') + QDir::toNativeSeparators(server) + QLatin1Char('"')},
                    {{QStringLiteral("PATH"),
                      msysBin + QLatin1Char(';') + qEnvironmentVariable("PATH")}});
    QSignalSpy finished(&queue, &RemoteTransferQueue::transferFinished);
    queue.enqueue(RemoteTransferQueue::Direction::Download,
                  {msysPath(QDir(remote).filePath(QStringLiteral("report[1].txt")))},
                  dir.filePath(QStringLiteral("download")));
    queue.enqueue(RemoteTransferQueue::Direction::Upload,
                  {QDir::toNativeSeparators(source.fileName())},
                  msysPath(dir.filePath(QStringLiteral("upload"))));
    // Retargeting the queue does not move jobs that are already queued.
    queue.setLaunch(QStringLiteral("missing-sftp.exe"), {}, {});
    QTRY_COMPARE_WITH_TIMEOUT(finished.size(), 2, 30000);
    QCOMPARE(failedTransfers(finished), QStringList{});
    QFile downloaded(dir.filePath(QStringLiteral("download/report[1].txt")));
    QVERIFY(downloaded.open(QIODevice::ReadOnly));
    QCOMPARE(downloaded.readAll(), QByteArray("down"));
    QVERIFY(!QFileInfo::exists(dir.filePath(QStringLiteral("download/report1.txt"))));
    QFile uploaded(dir.filePath(QStringLiteral("upload/report[1].txt")));
    QVERIFY(uploaded.open(QIODevice::ReadOnly));
    QCOMPARE(uploaded.readAll(), QByteArray("up"));
}

QTEST_GUILESS_MAIN(SshCommandTest)

#include "ssh_command_test.moc"
