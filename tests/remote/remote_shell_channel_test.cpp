#include "remote/remote_file_listing.h"
#include "remote/remote_file_model.h"
#include "remote/remote_shell_channel.h"
#include "remote/ssh_askpass.h"
#include "remote/ssh_command.h"
#include "property_check.h"

#include <QDateTime>
#include <QDeadlineTimer>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>
#include <QSet>
#include <QSignalSpy>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTest>
#include <QUrl>

#include <utility>

using namespace span::remote;

class RemoteShellChannelTest final : public QObject {
    Q_OBJECT

private slots:
    void parserSkipsLoginNoiseAndSplitChunks();
    void parserIgnoresForeignNonce();
    void parserReturnsBackToBackFrames();
    void parserReadsRandomStreamsAlikeAcrossRandomSplits();
    void listingParsesFindRecords();
    void listingRejectsPayloadWithoutDirectory();
    void pathHelpersStayInsideTheTree();
    void channelRunsScriptsOnPosixShell();
    void channelFailsRequestsWhenShellIsMissing();
    void fileModelDropsListingWhenEndpointChanges();
    void fileModelReconnectsWhenKeyChanges();
    void fileModelKeepsDirectoryWhenOpenFails();
    void fileModelKeepsDirectoryWhenListingFails();
    void fileModelExplainsMissingPromptBridge();
    void fileModelBrowsesSortsAndEditsADirectory();
    void fileModelRefusesUnsafeRequestsLocally();
};

namespace {

QByteArray frameFor(const QByteArray &nonce, quint64 id, const QByteArray &payload, int exitCode)
{
    const QByteArray tag = nonce + ':' + QByteArray::number(id);
    return '\x01' + tag + ":B\x02" + payload + '\x01' + tag + ":E:"
        + QByteArray::number(exitCode) + '\x02';
}

struct SplitStream {
    QByteArray bytes;
    QList<qsizetype> cuts;
};

// Frames for nonce "abc", foreign frames, login noise and stray marker
// fragments, cut at random points.
SplitStream randomFrameStream(QRandomGenerator &random)
{
    static const QList<QByteArray> alphabet{frameFor("abc", 1, "one", 0),
                                            frameFor("abc", 2, QByteArray("x\x02y\0z", 5), 3),
                                            frameFor("abc", 3, "", 1),
                                            frameFor("xyz", 4, "spoofed", 0),
                                            "Last login: now\r\n",
                                            "\x01",
                                            "\x02",
                                            "abc:",
                                            ":B",
                                            ":E:",
                                            "7"};
    SplitStream stream;
    stream.bytes = span::tests::tokenString(random, alphabet, 12);
    stream.cuts = span::tests::randomCuts(random, stream.bytes.size(), 10);
    return stream;
}

// "id exit payload-hex" per frame, then the bytes still buffered.
QStringList parsedFrames(const QList<QByteArray> &pieces)
{
    RemoteShellFrameParser parser("abc");
    QStringList parsed;
    for (const QByteArray &piece : pieces) {
        for (const RemoteShellFrameParser::Frame &frame : parser.feed(piece))
            parsed
                << QStringLiteral("%1 %2 %3").arg(frame.id).arg(frame.exitCode).arg(span::tests::hexOf(frame.payload));
    }
    parsed << QStringLiteral("buffered %1").arg(parser.bufferedBytes());
    return parsed;
}

bool parsesLikeWhole(const SplitStream &stream)
{
    return parsedFrames(span::tests::piecesAt(stream.bytes, stream.cuts)) == parsedFrames({stream.bytes});
}

QString describeSplit(const SplitStream &stream)
{
    QStringList cuts;
    for (qsizetype cut : stream.cuts) cuts.append(QString::number(cut));
    return QStringLiteral("bytes [%1] cuts [%2]").arg(span::tests::hexOf(stream.bytes), cuts.join(u' '));
}

QVector<RemoteShellFrameParser::Frame> feedBytewise(RemoteShellFrameParser &parser, const QByteArray &stream)
{
    QVector<RemoteShellFrameParser::Frame> frames;
    for (const char byte : stream) frames += parser.feed(QByteArray(1, byte));
    return frames;
}

// Entry names, sorted, with a trailing '/' on directories.
QStringList entryNames(const QVector<RemoteFileEntry> &entries)
{
    QStringList names;
    for (const RemoteFileEntry &entry : entries) names << entry.name + (entry.isDir ? QStringLiteral("/") : QString());
    names.sort();
    return names;
}

QStringList modelNames(const RemoteFileModel &model)
{
    QStringList names;
    for (int row = 0; row < model.count(); ++row)
        names << model.data(model.index(row), RemoteFileModel::NameRole).toString();
    return names;
}

// Creates an empty file at `path`, with its parent directories.
bool touchPath(const QString &path)
{
    QFile file(path);
    return QDir().mkpath(QFileInfo(path).absolutePath()) && file.open(QIODevice::WriteOnly);
}

// Git for Windows ships an MSYS sh plus GNU find: the same tools a Linux
// host offers to the file channel.
QString gitShell(QString *binDirectory)
{
    const QString git = QStandardPaths::findExecutable(QStringLiteral("git"));
    if (git.isEmpty()) {
        return {};
    }
    QDir dir = QFileInfo(git).absoluteDir();
    for (int depth = 0; depth < 3; ++depth) {
        const QString shell = dir.filePath(QStringLiteral("usr/bin/sh.exe"));
        if (QFileInfo(shell).isFile() && QFileInfo(dir.filePath(QStringLiteral("usr/bin/find.exe"))).isFile()) {
            *binDirectory = QDir::toNativeSeparators(dir.filePath(QStringLiteral("usr/bin")));
            return shell;
        }
        if (!dir.cdUp()) {
            break;
        }
    }
    return {};
}

struct Reply {
    bool ok = false;
    int exitCode = -1;
    QByteArray payload;
};

Reply run(RemoteShellChannel &channel, const QString &script)
{
    QSignalSpy replied(&channel, &RemoteShellChannel::replied);
    QSignalSpy failed(&channel, &RemoteShellChannel::requestFailed);
    const quint64 id = channel.submit(script);
    const QDeadlineTimer deadline(20000);
    while (!deadline.hasExpired()) {
        for (const QList<QVariant> &arguments : std::as_const(replied)) {
            if (arguments.at(0).toULongLong() == id) {
                return {true, arguments.at(1).toInt(), arguments.at(2).toByteArray()};
            }
        }
        for (const QList<QVariant> &arguments : std::as_const(failed)) {
            if (arguments.at(0).toULongLong() == id) {
                return {};
            }
        }
        QTest::qWait(10);
    }
    return {};
}

} // namespace

void RemoteShellChannelTest::parserSkipsLoginNoiseAndSplitChunks()
{
    RemoteShellFrameParser parser("abc");
    const QByteArray stream = "Welcome to host\r\nLast login: now\n"
        + frameFor("abc", 7, QByteArray("line1\nline\x02two\0end", 18), 3);
    const QVector<RemoteShellFrameParser::Frame> frames = feedBytewise(parser, stream);
    QCOMPARE(frames.size(), 1);
    QCOMPARE(frames.at(0).id, quint64(7));
    QCOMPARE(frames.at(0).exitCode, 3);
    QCOMPARE(frames.at(0).payload, QByteArray("line1\nline\x02two\0end", 18));
    QCOMPARE(parser.bufferedBytes(), 0);
}

void RemoteShellChannelTest::parserIgnoresForeignNonce()
{
    RemoteShellFrameParser parser("abc");
    QVERIFY(parser.feed(frameFor("xyz", 1, "spoofed", 0)).isEmpty());
    const auto frames = parser.feed(frameFor("abc", 2, "real", 0));
    QCOMPARE(frames.size(), 1);
    QCOMPARE(frames.at(0).payload, QByteArray("real"));
}

void RemoteShellChannelTest::parserReturnsBackToBackFrames()
{
    RemoteShellFrameParser parser("n");
    const auto frames = parser.feed(frameFor("n", 1, "a", 0) + frameFor("n", 2, "", 1));
    QCOMPARE(frames.size(), 2);
    QCOMPARE(frames.at(0).payload, QByteArray("a"));
    QCOMPARE(frames.at(1).id, quint64(2));
    QCOMPARE(frames.at(1).exitCode, 1);
    QVERIFY(frames.at(1).payload.isEmpty());

    // The wrapped script brackets its output with the same markers.
    const QByteArray wrapped = parser.wrap(5, QStringLiteral("ls"));
    QVERIFY(wrapped.contains("'n:5:B'"));
    QVERIFY(wrapped.contains("'n:5:E' \"$?\""));
    QVERIFY(wrapped.contains("</dev/null"));
}

void RemoteShellChannelTest::parserReadsRandomStreamsAlikeAcrossRandomSplits()
{
    QCOMPARE(span::tests::firstCounterexample(0xabc, 1000, randomFrameStream, parsesLikeWhole, describeSplit),
             QString());
}

void RemoteShellChannelTest::listingParsesFindRecords()
{
    QByteArray payload("/home/me");
    payload += '\0';
    payload += QByteArray("d\td\t4096\t1700000000.5\tdrwxr-xr-x\tme\tstaff\tsrc") + '\0';
    payload += QByteArray("l\td\t7\t1700000001.0\tlrwxrwxrwx\tme\tstaff\tlink") + '\0';
    payload += QByteArray("f\tf\t12\t1700000002.0\t-rw-r--r--\troot\troot\tname\twith tab") + '\0';
    payload += QByteArray("broken record") + '\0';

    RemoteListing listing;
    QVERIFY(parseRemoteListing(payload, &listing));
    QCOMPARE(listing.directory, QStringLiteral("/home/me"));
    QCOMPARE(listing.entries.size(), 3);
    QCOMPARE(listing.entries.at(0).path, QStringLiteral("/home/me/src"));
    QVERIFY(listing.entries.at(0).isDir);
    QVERIFY(listing.entries.at(1).isLink);
    QVERIFY(listing.entries.at(1).isDir);
    QCOMPARE(listing.entries.at(2).name, QStringLiteral("name\twith tab"));
    QCOMPARE(listing.entries.at(2).size, qint64(12));
    QCOMPARE(listing.entries.at(2).modifiedSecs, qint64(1700000002));
    QCOMPARE(listing.entries.at(2).owner, QStringLiteral("root"));
}

void RemoteShellChannelTest::listingRejectsPayloadWithoutDirectory()
{
    QVERIFY(!parseRemoteListing(QByteArray(), nullptr));
    QVERIFY(!parseRemoteListing(QByteArray("bash: cd: nope\n"), nullptr));
    // dash prefixes errors with its own path; without the NUL it is no listing.
    QVERIFY(!parseRemoteListing(QByteArray("/bin/sh: 1: cd: can't cd to /nope\n"), nullptr));
}

void RemoteShellChannelTest::pathHelpersStayInsideTheTree()
{
    QCOMPARE(normalizeRemotePath(QStringLiteral("//srv/./a/../b/")), QStringLiteral("/srv/b"));
    QCOMPARE(normalizeRemotePath(QStringLiteral("/../..")), QStringLiteral("/"));
    QCOMPARE(remoteParentPath(QStringLiteral("/srv/b")), QStringLiteral("/srv"));
    QCOMPARE(remoteParentPath(QStringLiteral("/srv")), QStringLiteral("/"));
    QCOMPARE(remoteChildPath(QStringLiteral("/"), QStringLiteral("etc")), QStringLiteral("/etc"));
    QVERIFY(isValidRemoteName(QStringLiteral("a b.txt")));
    QVERIFY(!isValidRemoteName(QStringLiteral("..")));
    QVERIFY(!isValidRemoteName(QStringLiteral("a/b")));
    QVERIFY(isRemovableRemotePath(QStringLiteral("/srv/data")));
    QVERIFY(!isRemovableRemotePath(QStringLiteral("/")));
    QVERIFY(!isRemovableRemotePath(QStringLiteral("srv")));
    QVERIFY(!isRemovableRemotePath(QStringLiteral("/srv/../")));
}

void RemoteShellChannelTest::channelRunsScriptsOnPosixShell()
{
    QString binDirectory;
    const QString shell = gitShell(&binDirectory);
    if (shell.isEmpty()) QSKIP("Git for Windows sh.exe with GNU find is not available");

    RemoteShellChannel channel;
    channel.setLaunch(shell, {},
                      {{QStringLiteral("PATH"),
                        binDirectory + QLatin1Char(';') + qEnvironmentVariable("PATH")}});
    // rc-file chatter before the first request must not disturb framing.
    const Reply home = run(channel, QStringLiteral("echo noise; ") + remoteHomeScript());
    QVERIFY(home.ok);
    QCOMPARE(home.exitCode, 0);
    QVERIFY(home.payload.startsWith("noise\n"));
    QVERIFY(home.payload.size() > 6);
    QCOMPARE(channel.state(), RemoteShellChannel::State::Ready);

    const Reply made = run(channel, QStringLiteral("d=$(mktemp -d) && printf '%s' \"$d\""));
    QVERIFY(made.ok);
    QCOMPARE(made.exitCode, 0);
    const QString root = QString::fromUtf8(made.payload);
    QVERIFY(root.startsWith(QLatin1Char('/')));

    const QString folder = remoteChildPath(root, QStringLiteral("sub dir"));
    const QString file = remoteChildPath(root, QStringLiteral("it's.txt"));
    const QString renamed = remoteChildPath(root, QStringLiteral("$(echo x).txt"));
    QCOMPARE(run(channel, remoteMakeDirectoryScript(folder)).exitCode, 0);
    QCOMPARE(run(channel, remoteCreateFileScript(file)).exitCode, 0);
    QCOMPARE(run(channel, remoteCreateFileScript(file)).exitCode, kRemoteExistsExitCode);
    QCOMPARE(run(channel, remoteRenameScript(file, renamed)).exitCode, 0);
    QCOMPARE(run(channel, remoteRenameScript(renamed, folder)).exitCode, kRemoteExistsExitCode);

    const Reply listed = run(channel, remoteListScript(root));
    QVERIFY(listed.ok);
    QCOMPARE(listed.exitCode, 0);
    RemoteListing listing;
    QVERIFY(parseRemoteListing(listed.payload, &listing));
    QCOMPARE(listing.directory, root);
    QCOMPARE(entryNames(listing.entries), (QStringList{QStringLiteral("$(echo x).txt"), QStringLiteral("sub dir/")}));

    QCOMPARE(run(channel, remoteListScript(remoteChildPath(root, QStringLiteral("missing")))).exitCode, 2);
    QCOMPARE(run(channel, remoteRemoveScript({root})).exitCode, 0);
    QCOMPARE(run(channel, remoteListScript(root)).exitCode, 2);

    channel.stop();
    QCOMPARE(channel.state(), RemoteShellChannel::State::Idle);

    // The next request starts the shell again. When the connection drops
    // mid-request (here the shell itself is killed; requests run in a
    // subshell), the request fails with what the client last printed on
    // stderr (ssh's own reason).
    QSignalSpy failed(&channel, &RemoteShellChannel::requestFailed);
    const quint64 dropped = channel.submit(
        QStringLiteral("echo 'Connection reset by peer' >&2; kill -9 $$"));
    QTRY_COMPARE_WITH_TIMEOUT(failed.size(), 1, 10000);
    QCOMPARE(failed.at(0).at(0).toULongLong(), dropped);
    QVERIFY(failed.at(0).at(1).toString().contains(QStringLiteral("Connection reset by peer")));
    QCOMPARE(channel.state(), RemoteShellChannel::State::Failed);
}

void RemoteShellChannelTest::channelFailsRequestsWhenShellIsMissing()
{
    RemoteShellChannel channel;
    channel.setLaunch(QString(), {}, {});
    QSignalSpy failed(&channel, &RemoteShellChannel::requestFailed);
    const quint64 id = channel.submit(remoteHomeScript());
    QTRY_COMPARE(failed.size(), 1);
    QCOMPARE(failed.at(0).at(0).toULongLong(), id);
    QCOMPARE(channel.state(), RemoteShellChannel::State::Failed);
    QVERIFY(!channel.errorMessage().isEmpty());
}

void RemoteShellChannelTest::fileModelDropsListingWhenEndpointChanges()
{
    QString binDirectory;
    const QString shell = gitShell(&binDirectory);
    if (shell.isEmpty()) QSKIP("Git for Windows sh.exe with GNU find is not available");
    // Port 1 on loopback: a reconnect is refused locally at once.
    const SshEndpoint first{QStringLiteral("127.0.0.1"), 1, QStringLiteral("alice"), QString()};
    RemoteFileModel model;
    model.configure(first, {});
    // Stand the POSIX shell in for host A.
    model.channel()->setLaunch(shell, {},
                               {{QStringLiteral("PATH"),
                                 binDirectory + QLatin1Char(';') + qEnvironmentVariable("PATH")}});
    model.activate();
    QTRY_VERIFY_WITH_TIMEOUT(!model.currentPath().isEmpty(), 15000);
    const QString listed = model.currentPath();

    // Saving the host unchanged keeps the listing and the connection.
    model.configure(first, {});
    QCOMPARE(model.currentPath(), listed);
    QCOMPARE(model.channel()->state(), RemoteShellChannel::State::Ready);

    QSignalSpy pathChanged(&model, &RemoteFileModel::currentPathChanged);
    model.configure({QStringLiteral("127.0.0.1"), 1, QStringLiteral("bob"), QString()}, {});
    QVERIFY(model.currentPath().isEmpty());
    QCOMPARE(model.count(), 0);
    QCOMPARE(pathChanged.size(), 1);
    // Reconnecting to the new endpoint, not to the old shell.
    QVERIFY(model.loading());
    QVERIFY(model.channel()->state() != RemoteShellChannel::State::Ready);
}

void RemoteShellChannelTest::fileModelReconnectsWhenKeyChanges()
{
    QString binDirectory;
    const QString shell = gitShell(&binDirectory);
    if (shell.isEmpty()) QSKIP("Git for Windows sh.exe with GNU find is not available");
    // The key copy keeps its path when another key is imported into it.
    const SshEndpoint endpoint{QStringLiteral("127.0.0.1"), 1, QStringLiteral("alice"),
                               QStringLiteral("C:/keys/host-id")};
    RemoteFileModel model;
    model.configure(endpoint, {});
    model.channel()->setLaunch(shell, {},
                               {{QStringLiteral("PATH"),
                                 binDirectory + QLatin1Char(';') + qEnvironmentVariable("PATH")}});
    model.activate();
    QTRY_VERIFY_WITH_TIMEOUT(!model.currentPath().isEmpty(), 15000);

    model.configure(endpoint, {}, true);
    QVERIFY(model.currentPath().isEmpty());
    QCOMPARE(model.count(), 0);
    QVERIFY(model.loading());
    QVERIFY(model.channel()->state() != RemoteShellChannel::State::Ready);
}

void RemoteShellChannelTest::fileModelKeepsDirectoryWhenOpenFails()
{
    QString binDirectory;
    const QString shell = gitShell(&binDirectory);
    if (shell.isEmpty()) QSKIP("Git for Windows sh.exe with GNU find is not available");
    RemoteFileModel model;
    model.configure({QStringLiteral("127.0.0.1"), 1, QStringLiteral("alice"), QString()}, {});
    model.channel()->setLaunch(shell, {},
                               {{QStringLiteral("PATH"),
                                 binDirectory + QLatin1Char(';') + qEnvironmentVariable("PATH")}});
    model.activate();
    QTRY_VERIFY_WITH_TIMEOUT(!model.currentPath().isEmpty(), 15000);
    QTRY_VERIFY_WITH_TIMEOUT(!model.loading(), 15000);
    const QString listed = model.currentPath();
    const int rows = model.count();

    QSignalSpy pathChanged(&model, &RemoteFileModel::currentPathChanged);
    model.openPath(remoteChildPath(listed, QStringLiteral("no such directory")));
    QTRY_VERIFY_WITH_TIMEOUT(!model.errorMessage().isEmpty(), 15000);
    QVERIFY(!model.loading());
    QCOMPARE(model.currentPath(), listed);
    QCOMPARE(model.count(), rows);
    QCOMPARE(pathChanged.size(), 0);
}

void RemoteShellChannelTest::fileModelKeepsDirectoryWhenListingFails()
{
    QString binDirectory;
    const QString shell = gitShell(&binDirectory);
    if (shell.isEmpty()) QSKIP("Git for Windows sh.exe with GNU find is not available");
    // A find in front of the real one: in a directory holding ".deny" it
    // fails like GNU find on a directory that may be entered but not read
    // (mode 711); with ".nofind" it behaves like a find without -printf.
    QTemporaryDir root;
    QVERIFY(root.isValid());
    QFile fake(root.filePath(QStringLiteral("bin/find")));
    QVERIFY(QDir().mkpath(root.filePath(QStringLiteral("bin"))));
    QVERIFY(fake.open(QIODevice::WriteOnly));
    fake.write("#!/bin/sh\n"
               "if [ -e .nofind ]; then echo 'find: unknown predicate -printf' >&2; exit 1; fi\n"
               "case \" $* \" in *' -mindepth 1 '*)\n"
               "  if [ -e .deny ]; then echo \"find: '.': Permission denied\" >&2; exit 1; fi;;\n"
               "esac\n"
               "exec /usr/bin/find \"$@\"\n");
    fake.close();
    QVERIFY(touchPath(root.filePath(QStringLiteral("locked/.deny"))));
    QVERIFY(touchPath(root.filePath(QStringLiteral("plain/.nofind"))));
    // C:/x is /c/x to the MSYS shell.
    const auto posix = [&](const QString &name) {
        const QString path = root.filePath(name);
        return QLatin1Char('/') + path.left(1).toLower() + path.mid(2);
    };

    RemoteFileModel model;
    model.configure({QStringLiteral("127.0.0.1"), 1, QStringLiteral("alice"), QString()}, {});
    model.channel()->setLaunch(shell, {},
                               {{QStringLiteral("PATH"),
                                 QDir::toNativeSeparators(root.filePath(QStringLiteral("bin")))
                                     + QLatin1Char(';') + binDirectory + QLatin1Char(';')
                                     + qEnvironmentVariable("PATH")}});
    model.openPath(posix(QStringLiteral("bin")));
    QTRY_VERIFY_WITH_TIMEOUT(!model.currentPath().isEmpty() && !model.loading(), 15000);
    QCOMPARE(model.errorMessage(), QString());
    QCOMPARE(model.count(), 1);
    const QString listed = model.currentPath();

    QSignalSpy pathChanged(&model, &RemoteFileModel::currentPathChanged);
    model.openPath(posix(QStringLiteral("locked")));
    QTRY_VERIFY_WITH_TIMEOUT(!model.errorMessage().isEmpty(), 15000);
    QVERIFY2(model.errorMessage().contains(QStringLiteral("Permission denied")),
             qPrintable(model.errorMessage()));
    QCOMPARE(model.currentPath(), listed);
    QCOMPARE(model.count(), 1);

    model.openPath(posix(QStringLiteral("plain")));
    QTRY_VERIFY_WITH_TIMEOUT(model.errorMessage().contains(QStringLiteral("GNU find")), 15000);
    QCOMPARE(model.currentPath(), listed);
    QCOMPARE(model.count(), 1);
    QCOMPARE(pathChanged.size(), 0);
}

void RemoteShellChannelTest::fileModelExplainsMissingPromptBridge()
{
    const SshEndpoint endpoint{QStringLiteral("127.0.0.1"), 1, QStringLiteral("alice"), QString()};
    const auto connectionError = [&endpoint](const QHash<QString, QString> &environment) {
        RemoteFileModel model;
        model.configure(endpoint, environment);
        // No program: the channel fails at once with its own message.
        model.channel()->setLaunch(QString(), {}, environment);
        model.activate();
        // Empty when no error arrives in time.
        return QTest::qWaitFor([&model]() { return !model.errorMessage().isEmpty(); }, 5000) ? model.errorMessage()
                                                                                             : QString();
    };

    const QString bridged = connectionError(
        {{QString::fromLatin1(kAskpassPipeEnv), QStringLiteral("SpanTui-askpass-test")}});
    QVERIFY(!bridged.isEmpty());
    QVERIFY(!bridged.contains(QStringLiteral("prompt bridge")));
    // Without the askpass environment ssh could not have asked anything:
    // the same failure says so and how to get around it.
    const QString unbridged = connectionError({});
    QVERIFY(unbridged.startsWith(bridged));
    QVERIFY(unbridged.contains(QStringLiteral("prompt bridge")));
}

void RemoteShellChannelTest::fileModelBrowsesSortsAndEditsADirectory()
{
    QString binDirectory;
    const QString shell = gitShell(&binDirectory);
    if (shell.isEmpty()) QSKIP("Git for Windows sh.exe with GNU find is not available");
    QTemporaryDir root;
    QVERIFY(root.isValid());
    const auto write = [&root](const QString &name, const QByteArray &bytes) {
        QFile file(root.filePath(name));
        QDir().mkpath(QFileInfo(file).absolutePath());
        return file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size();
    };
    QVERIFY(write(QStringLiteral("sub/inner.txt"), "x"));
    QVERIFY(write(QStringLiteral("small.txt"), "abc"));
    QVERIFY(write(QStringLiteral("large.log"), "0123456789"));
    QVERIFY(write(QStringLiteral(".hidden"), "h"));
    const auto age = [&root](const QString &name, int days) {
        QFile file(root.filePath(name));
        return file.open(QIODevice::ReadWrite)
            && file.setFileTime(QDateTime::currentDateTime().addDays(-days),
                                QFileDevice::FileModificationTime);
    };
    QVERIFY(age(QStringLiteral("small.txt"), 2));
    QVERIFY(age(QStringLiteral("large.log"), 1));
    const QString top = QLatin1Char('/') + root.path().left(1).toLower() + root.path().mid(2);

    RemoteFileModel model;
    model.configure({QStringLiteral("127.0.0.1"), 1, QStringLiteral("alice"), QString()}, {});
    model.channel()->setLaunch(shell, {},
                               {{QStringLiteral("PATH"),
                                 binDirectory + QLatin1Char(';') + qEnvironmentVariable("PATH")}});
    const auto names = [&model]() { return modelNames(model); };
    const auto rowOf = [&](const QString &name) { return int(names().indexOf(name)); };
    const auto settled = [&model]() { return !model.loading(); };

    model.openPath(top);
    QTRY_VERIFY_WITH_TIMEOUT(model.currentPath() == top && settled(), 15000);
    QCOMPARE(model.connectionState(), QStringLiteral("ready"));
    // Folders first, then names; hidden entries only on request.
    QCOMPARE(names(), (QStringList{QStringLiteral("sub"), QStringLiteral("large.log"),
                                   QStringLiteral("small.txt")}));
    QVERIFY(model.data(model.index(0), RemoteFileModel::IsDirRole).toBool());
    QVERIFY(model.data(model.index(0), RemoteFileModel::SizeTextRole).toString().isEmpty());
    QVERIFY(!model.data(model.index(1), RemoteFileModel::SizeTextRole).toString().isEmpty());
    const QModelIndex small = model.index(rowOf(QStringLiteral("small.txt")));
    QCOMPARE(model.data(small, RemoteFileModel::PathRole).toString(),
             remoteChildPath(top, QStringLiteral("small.txt")));
    QVERIFY(!model.data(small, RemoteFileModel::IsLinkRole).toBool());
    QVERIFY(!model.data(small, RemoteFileModel::PermissionsRole).toString().isEmpty());
    QVERIFY(QRegularExpression(QStringLiteral(R"(^\d{4}-\d\d-\d\d \d\d:\d\d$)"))
                .match(model.data(small, RemoteFileModel::ModifiedTextRole).toString())
                .hasMatch());
    model.setShowHidden(true);
    QCOMPARE(rowOf(QStringLiteral(".hidden")) >= 0, true);
    model.setShowHidden(false);

    model.sortBy(QStringLiteral("size"));
    QCOMPARE(names(), (QStringList{QStringLiteral("sub"), QStringLiteral("small.txt"),
                                   QStringLiteral("large.log")}));
    model.sortBy(QStringLiteral("size"));
    QVERIFY(!model.sortAscending());
    QCOMPARE(names(), (QStringList{QStringLiteral("sub"), QStringLiteral("large.log"),
                                   QStringLiteral("small.txt")}));
    model.sortBy(QStringLiteral("modified"));
    QCOMPARE(names(), (QStringList{QStringLiteral("sub"), QStringLiteral("small.txt"),
                                   QStringLiteral("large.log")}));
    // Unknown keys leave the order alone.
    model.sortBy(QStringLiteral("owner"));
    QCOMPARE(model.sortKey(), QStringLiteral("modified"));
    model.sortBy(QStringLiteral("name"));

    model.createFolder(QStringLiteral("made"));
    QTRY_VERIFY_WITH_TIMEOUT(rowOf(QStringLiteral("made")) >= 0, 15000);
    QVERIFY(QFileInfo(root.filePath(QStringLiteral("made"))).isDir());
    // A refused mutation reports the host's own words, or the known reason.
    model.createFolder(QStringLiteral("made"));
    QTRY_VERIFY_WITH_TIMEOUT(model.errorMessage().contains(QStringLiteral("File exists")), 15000);
    model.clearError();
    model.createFile(QStringLiteral("small.txt"));
    QTRY_VERIFY_WITH_TIMEOUT(model.errorMessage().contains(QStringLiteral("already exists")), 15000);
    model.clearError();

    model.createFile(QStringLiteral("new.txt"));
    QTRY_VERIFY_WITH_TIMEOUT(rowOf(QStringLiteral("new.txt")) >= 0, 15000);
    model.rename(remoteChildPath(top, QStringLiteral("new.txt")), QStringLiteral("renamed.txt"));
    QTRY_VERIFY_WITH_TIMEOUT(rowOf(QStringLiteral("renamed.txt")) >= 0, 15000);
    QVERIFY(QFileInfo::exists(root.filePath(QStringLiteral("renamed.txt"))));
    model.remove({remoteChildPath(top, QStringLiteral("renamed.txt"))});
    QTRY_VERIFY_WITH_TIMEOUT(rowOf(QStringLiteral("renamed.txt")) < 0 && settled(), 15000);
    QVERIFY(!QFileInfo::exists(root.filePath(QStringLiteral("renamed.txt"))));
    QCOMPARE(model.errorMessage(), QString());

    model.openRow(rowOf(QStringLiteral("sub")));
    QTRY_VERIFY_WITH_TIMEOUT(model.currentPath() == remoteChildPath(top, QStringLiteral("sub"))
                                 && settled(), 15000);
    QCOMPARE(names(), QStringList{QStringLiteral("inner.txt")});
    model.goUp();
    QTRY_VERIFY_WITH_TIMEOUT(model.currentPath() == top && settled(), 15000);

    // A relative path opens below the current folder, and a reconnect lists
    // the same folder again on a fresh shell.
    const QString sub = remoteChildPath(top, QStringLiteral("sub"));
    model.openPath(QStringLiteral("sub"));
    QTRY_VERIFY_WITH_TIMEOUT(model.currentPath() == sub && settled(), 15000);
    model.reconnect();
    QTRY_VERIFY_WITH_TIMEOUT(model.connectionState() == QStringLiteral("ready") && settled(),
                             15000);
    QCOMPARE(model.currentPath(), sub);
    QCOMPARE(names(), QStringList{QStringLiteral("inner.txt")});
}

void RemoteShellChannelTest::fileModelRefusesUnsafeRequestsLocally()
{
    QTemporaryDir root;
    QVERIFY(root.isValid());
    RemoteFileModel model;
    QCOMPARE(model.connectionState(), QStringLiteral("idle"));
    const QList<QByteArray> roles = model.roleNames().values();
    QVERIFY(QSet<QByteArray>(roles.cbegin(), roles.cend())
                .contains(QSet<QByteArray>{"name", "path", "isDir", "isLink", "sizeText", "modifiedText", "permissions",
                                           "owner"}));

    // Each request is refused before any script is built, so nothing starts.
    model.createFolder(QStringLiteral("a/b"));
    QVERIFY(model.errorMessage().contains(QStringLiteral("a/b")));
    model.createFile(QStringLiteral(".."));
    QVERIFY(model.errorMessage().contains(QStringLiteral("..")));
    model.rename(QStringLiteral("/"), QStringLiteral("root"));
    QVERIFY(model.errorMessage().contains(QStringLiteral("root")));
    model.rename(QStringLiteral("/srv/../etc"), QStringLiteral("passwd"));
    QVERIFY(model.errorMessage().contains(QStringLiteral("passwd")));
    model.clearError();
    model.remove({QStringLiteral("/home/alice/ok"), QStringLiteral("/")});
    QVERIFY(model.errorMessage().contains(QStringLiteral("Refusing to delete")));
    model.clearError();
    model.download({QStringLiteral("/srv/file")},
                   QUrl::fromLocalFile(root.filePath(QStringLiteral("missing"))));
    QVERIFY(!model.errorMessage().isEmpty());
    QSignalSpy sorted(&model, &RemoteFileModel::sortChanged);
    model.sortBy(QStringLiteral("owner"));
    QCOMPARE(sorted.count(), 0);
    QCOMPARE(model.connectionState(), QStringLiteral("idle"));
    QVERIFY(!model.loading());
}

QTEST_GUILESS_MAIN(RemoteShellChannelTest)

#include "remote_shell_channel_test.moc"
