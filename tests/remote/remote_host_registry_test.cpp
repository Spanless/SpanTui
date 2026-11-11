#include "core/workspace_database_migration.h"
#include "remote/remote_host_registry.h"
#include "remote/ssh_askpass.h"

#include <QDeadlineTimer>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QRegularExpression>
#include <QSignalSpy>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QTemporaryDir>
#include <QTest>

#include <future>

#include <qt_windows.h>
#include <aclapi.h>

using span::remote::RemoteHostRegistry;
using span::remote::SshAskpassBroker;

class RemoteHostRegistryTest final : public QObject {
    Q_OBJECT

private slots:
    void savesHostsAndEncryptsSecrets();
    void editingKeepsOrClearsSavedSecrets();
    void movingDropsSavedPassword();
    void rejectsInvalidHost();
    void savedPasswordAnswersFirstPromptOnly();
    void typedAnswerResolvesQueuedDuplicates();
    void trustedHostKeyIsAnsweredOnce();
    void terminalLaunchUsesAskpassAndPortableTerm();
    void importsOwnerOnlyKeyCopy();
    void failedStoreKeepsKeyCopy();
    void failedKeySwapRollsBackSave();
    void keyCopyInUseIsDeletedOnNextStart();
    void unreadableListPrunesNothing();
    void keyPathFromPickedFile();
    void emptyUserMeansRoot();
    void secretsGoOnlyToTheirOwnPrompts();
    void failedSaveKeepsSessionSecrets();
    void reportsPromptBridgeState();
    void newDestinationRevokesOldLaunches();
};

namespace {

// The registry reads the schema owned by WorkspaceDatabaseMigration, which
// the workspace runs before any host connection opens.
void prepareSchema(const QString &databasePath)
{
    const QString connection = QStringLiteral("remote_host_registry_test_migration");
    {
        QSqlDatabase database = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), connection);
        database.setDatabaseName(databasePath);
        QString error;
        QVERIFY(database.open());
        QVERIFY2(span::core::migrateWorkspaceDatabase(database, &error), qPrintable(error));
    }
    QSqlDatabase::removeDatabase(connection);
}

QVariantMap hostFields(const QString &host, const QString &password = {})
{
    return {
        {QStringLiteral("host"), host},
        {QStringLiteral("port"), 22},
        {QStringLiteral("user"), QStringLiteral("alice")},
        {QStringLiteral("password"), password},
    };
}

QByteArray storedPassword(const QString &databasePath)
{
    QByteArray blob;
    {
        QSqlDatabase db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), QStringLiteral("peek"));
        db.setDatabaseName(databasePath);
        if (db.open()) {
            QSqlQuery query(db);
            if (query.exec(QStringLiteral("select password from remote_hosts")) && query.next()) {
                blob = query.value(0).toByteArray();
            }
        }
    }
    QSqlDatabase::removeDatabase(QStringLiteral("peek"));
    return blob;
}

bool writeFile(const QString &path, const QByteArray &data)
{
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile file(path);
    return file.open(QIODevice::WriteOnly) && file.write(data) == data.size();
}

QByteArray readFile(const QString &path)
{
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
}

// What OpenSSH checks: a protected DACL whose only ACE allows the current user.
bool isOwnerOnly(const QString &path)
{
    PACL dacl = nullptr;
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    const std::wstring native = QDir::toNativeSeparators(path).toStdWString();
    if (GetNamedSecurityInfoW(native.c_str(), SE_FILE_OBJECT, DACL_SECURITY_INFORMATION, nullptr, nullptr, &dacl,
                              nullptr, &descriptor)
        != ERROR_SUCCESS) {
        return false;
    }
    SECURITY_DESCRIPTOR_CONTROL control = 0;
    DWORD revision = 0;
    bool ownerOnly = GetSecurityDescriptorControl(descriptor, &control, &revision) && (control & SE_DACL_PROTECTED)
        && dacl && dacl->AceCount == 1;
    void *ace = nullptr;
    ownerOnly =
        ownerOnly && GetAce(dacl, 0, &ace) && static_cast<ACE_HEADER *>(ace)->AceType == ACCESS_ALLOWED_ACE_TYPE;
    if (ownerOnly) {
        HANDLE token = nullptr;
        OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token);
        QByteArray user(512, '\0');
        DWORD needed = 0;
        ownerOnly = token && GetTokenInformation(token, TokenUser, user.data(), DWORD(user.size()), &needed)
            && EqualSid(&static_cast<ACCESS_ALLOWED_ACE *>(ace)->SidStart,
                        reinterpret_cast<TOKEN_USER *>(user.data())->User.Sid);
        if (token) {
            CloseHandle(token);
        }
    }
    LocalFree(descriptor);
    return ownerOnly;
}

struct Exchange {
    bool ok = false;
    QString answer;
};

std::future<Exchange> ask(const QVariantMap &environment, quint32 parentPid, const QString &prompt)
{
    const QString pipe = environment.value(QLatin1String(span::remote::kAskpassPipeEnv)).toString();
    const QString token = environment.value(QLatin1String(span::remote::kAskpassTokenEnv)).toString();
    return std::async(std::launch::async, [=]() {
        Exchange result;
        result.ok = span::remote::exchangeAskpassPrompt(pipe, token, parentPid, prompt, &result.answer);
        return result;
    });
}

bool settle(std::future<Exchange> &future, Exchange *result)
{
    const QDeadlineTimer deadline(10000);
    while (future.wait_for(std::chrono::seconds(0)) != std::future_status::ready) {
        if (deadline.hasExpired()) {
            return false;
        }
        QTest::qWait(10);
    }
    *result = future.get();
    return true;
}

// Stand-ins for ssh processes the app started: saved secrets only answer
// prompts whose asking process is a child of this process. Any other pid
// plays a ProxyJump hop.
class LaunchedSsh {
public:
    ~LaunchedSsh()
    {
        for (QProcess *process : std::as_const(m_processes)) {
            process->kill();
            process->waitForFinished();
            delete process;
        }
    }
    quint32 start()
    {
        auto *process = new QProcess;
        m_processes << process;
        process->start(QStringLiteral("ping.exe"),
                       {QStringLiteral("-n"), QStringLiteral("60"), QStringLiteral("127.0.0.1")});
        return process->waitForStarted() ? quint32(process->processId()) : 0;
    }

private:
    QList<QProcess *> m_processes;
};

bool waitForPrompt(const RemoteHostRegistry &registry)
{
    const QDeadlineTimer deadline(10000);
    while (registry.pendingPrompt().isEmpty() && !deadline.hasExpired()) {
        QTest::qWait(10);
    }
    return !registry.pendingPrompt().isEmpty();
}

// Answers the next prompt with `answer` (asking to remember it) when it is
// of `kind` and offers no remembering; false otherwise.
bool answerPending(RemoteHostRegistry &registry, const QString &kind, const QString &answer)
{
    if (!waitForPrompt(registry)) return false;
    const QVariantMap pending = registry.pendingPrompt();
    if (pending.value(QStringLiteral("kind")).toString() != kind
        || pending.value(QStringLiteral("canRemember")).toBool())
        return false;
    registry.answerPrompt(pending.value(QStringLiteral("requestId")).toString(), true, answer, true);
    return true;
}

} // namespace

void RemoteHostRegistryTest::savesHostsAndEncryptsSecrets()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString databasePath = dir.filePath(QStringLiteral("state.db"));
    prepareSchema(databasePath);

    QString id;
    {
        RemoteHostRegistry registry(databasePath, nullptr);
        QCOMPARE(registry.count(), 0);
        QSignalSpy changed(&registry, &RemoteHostRegistry::hostsChanged);
        QSignalSpy errors(&registry, &RemoteHostRegistry::errorOccurred);
        id = registry.saveHost(hostFields(QStringLiteral("example.com"), QStringLiteral("hunter2")));
        QVERIFY2(!id.isEmpty(), qPrintable(errors.isEmpty() ? QString() : errors.constFirst().constFirst().toString()));
        QCOMPARE(changed.size(), 1);
        QCOMPARE(registry.hostName(id), QStringLiteral("example.com"));
        QCOMPARE(registry.data(registry.index(0), RemoteHostRegistry::AddressRole).toString(),
                 QStringLiteral("alice@example.com"));
        QVERIFY(registry.setExpanded(id, false));
    }

    const QByteArray blob = storedPassword(databasePath);
    QVERIFY(!blob.isEmpty());
    QVERIFY(!blob.contains("hunter2"));
    QVERIFY(!blob.contains(QByteArray(reinterpret_cast<const char *>(u"hunter2"), 14)));

    RemoteHostRegistry reloaded(databasePath, nullptr);
    QCOMPARE(reloaded.hostIds(), QStringList{id});
    // Saved hosts are found by address, ignoring case and empty candidates.
    QCOMPARE(reloaded.hostIdForAddress({QString(), QStringLiteral("EXAMPLE.com")}), id);
    QVERIFY(reloaded.hostIdForAddress({QStringLiteral("100.64.0.5")}).isEmpty());
    const QVariantMap details = reloaded.hostDetails(id);
    QCOMPARE(details.value(QStringLiteral("user")).toString(), QStringLiteral("alice"));
    QVERIFY(details.value(QStringLiteral("hasPassword")).toBool());
    QVERIFY(!details.contains(QStringLiteral("password")));
    QCOMPARE(reloaded.data(reloaded.index(0), RemoteHostRegistry::ExpandedRole).toBool(), false);

    // A non-default port shows in the address.
    QVariantMap moved = hostFields(QStringLiteral("example.com"));
    moved.insert(QStringLiteral("id"), id);
    moved.insert(QStringLiteral("port"), 2222);
    QCOMPARE(reloaded.saveHost(moved), id);
    QCOMPARE(reloaded.data(reloaded.index(0), RemoteHostRegistry::AddressRole).toString(),
             QStringLiteral("alice@example.com:2222"));
    span::remote::SshEndpoint endpoint;
    QVERIFY(reloaded.endpointFor(id, &endpoint));
    QCOMPARE(endpoint.port, 2222);

    QVERIFY(reloaded.removeHost(id));
    QCOMPARE(reloaded.count(), 0);
    RemoteHostRegistry emptied(databasePath, nullptr);
    QCOMPARE(emptied.count(), 0);
}

void RemoteHostRegistryTest::editingKeepsOrClearsSavedSecrets()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString databasePath = dir.filePath(QStringLiteral("state.db"));
    prepareSchema(databasePath);

    RemoteHostRegistry registry(databasePath, nullptr);
    const QString id = registry.saveHost(hostFields(QStringLiteral("h"), QStringLiteral("pw")));
    // A rename keeps the password.
    QVariantMap edit = hostFields(QStringLiteral("h"));
    edit.insert(QStringLiteral("id"), id);
    edit.insert(QStringLiteral("name"), QStringLiteral("Build box"));
    QVERIFY(!registry.movesHost(edit));
    QCOMPARE(registry.saveHost(edit), id);
    QCOMPARE(registry.count(), 1);
    QCOMPARE(registry.hostName(id), QStringLiteral("Build box"));
    QVERIFY(registry.hostDetails(id).value(QStringLiteral("hasPassword")).toBool());

    edit.insert(QStringLiteral("clearPassword"), true);
    QCOMPARE(registry.saveHost(edit), id);
    QVERIFY(!registry.hostDetails(id).value(QStringLiteral("hasPassword")).toBool());
    QVERIFY(storedPassword(databasePath).isEmpty());
}

void RemoteHostRegistryTest::movingDropsSavedPassword()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString databasePath = dir.filePath(QStringLiteral("state.db"));
    prepareSchema(databasePath);

    RemoteHostRegistry registry(databasePath, nullptr);
    const QString id = registry.saveHost(hostFields(QStringLiteral("h"), QStringLiteral("pw")));
    QVariantMap edit = hostFields(QStringLiteral("h2"));
    edit.insert(QStringLiteral("id"), id);
    QVERIFY(registry.movesHost(edit));
    QCOMPARE(registry.saveHost(edit), id);
    QVERIFY(!registry.hostDetails(id).value(QStringLiteral("hasPassword")).toBool());
    QVERIFY(storedPassword(databasePath).isEmpty());

    // A password typed with the move is saved for the new server.
    QVariantMap back = hostFields(QStringLiteral("h"), QStringLiteral("new"));
    back.insert(QStringLiteral("id"), id);
    QVERIFY(registry.movesHost(back));
    QCOMPARE(registry.saveHost(back), id);
    QVERIFY(registry.hostDetails(id).value(QStringLiteral("hasPassword")).toBool());

    // An empty user is root either way, so it is not a move.
    QVariantMap root = hostFields(QStringLiteral("h"));
    root.insert(QStringLiteral("id"), id);
    root.insert(QStringLiteral("user"), QStringLiteral("root"));
    QVariantMap blank = root;
    blank.insert(QStringLiteral("user"), QString());
    QCOMPARE(registry.saveHost(root), id);
    QVERIFY(!registry.movesHost(blank));
}

void RemoteHostRegistryTest::rejectsInvalidHost()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString databasePath = dir.filePath(QStringLiteral("state.db"));
    prepareSchema(databasePath);

    RemoteHostRegistry registry(databasePath, nullptr);
    QSignalSpy errors(&registry, &RemoteHostRegistry::errorOccurred);
    QVERIFY(!registry.validationError(hostFields(QStringLiteral("-oProxyCommand=x"))).isEmpty());
    QVERIFY(registry.saveHost(hostFields(QStringLiteral("-oProxyCommand=x"))).isEmpty());
    QCOMPARE(errors.size(), 1);
    QCOMPARE(registry.count(), 0);

    QVariantMap fields = hostFields(QStringLiteral("example.com"));
    fields.insert(QStringLiteral("port"), 70000);
    QVERIFY(registry.validationError(fields).contains(QStringLiteral("port")));
    fields = hostFields(QStringLiteral("example.com"));
    fields.insert(QStringLiteral("user"), QStringLiteral("a b;c"));
    QVERIFY(registry.validationError(fields).contains(QStringLiteral("user name")));
    fields = hostFields(QStringLiteral("example.com"));
    fields.insert(QStringLiteral("identityFile"), dir.filePath(QStringLiteral("missing_key")));
    QVERIFY(registry.validationError(fields).contains(QStringLiteral("private key")));
}

void RemoteHostRegistryTest::savedPasswordAnswersFirstPromptOnly()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString databasePath = dir.filePath(QStringLiteral("state.db"));
    prepareSchema(databasePath);
    const QString prompt = QStringLiteral("alice@h's password: ");
    LaunchedSsh launched;
    const quint32 ssh = launched.start();
    QVERIFY(ssh != 0);

    QString id;
    {
        SshAskpassBroker broker;
        QVERIFY(broker.listen());
        RemoteHostRegistry registry(databasePath, &broker);
        id = registry.saveHost(hostFields(QStringLiteral("h"), QStringLiteral("old")));
        const QVariantMap environment = registry.terminalEnvironment(id);

        Exchange result;
        auto first = ask(environment, ssh, prompt);
        QVERIFY(settle(first, &result));
        QVERIFY(result.ok);
        QCOMPARE(result.answer, QStringLiteral("old"));
        QVERIFY(registry.pendingPrompt().isEmpty());

        // ssh asks again: the saved password was wrong, so the user decides.
        auto retry = ask(environment, ssh, prompt);
        QVERIFY(waitForPrompt(registry));
        const QVariantMap pending = registry.pendingPrompt();
        QCOMPARE(pending.value(QStringLiteral("kind")).toString(), QStringLiteral("password"));
        QVERIFY(pending.value(QStringLiteral("retry")).toBool());
        QCOMPARE(pending.value(QStringLiteral("hostName")).toString(), QStringLiteral("h"));
        registry.answerPrompt(pending.value(QStringLiteral("requestId")).toString(), true, QStringLiteral("new"), true);
        QVERIFY(settle(retry, &result));
        QCOMPARE(result.answer, QStringLiteral("new"));
        QVERIFY(registry.pendingPrompt().isEmpty());
    }

    // The remembered answer survives a restart and is offered first.
    SshAskpassBroker broker;
    QVERIFY(broker.listen());
    RemoteHostRegistry registry(databasePath, &broker);
    Exchange result;
    auto next = ask(registry.terminalEnvironment(id), launched.start(), prompt);
    QVERIFY(settle(next, &result));
    QCOMPARE(result.answer, QStringLiteral("new"));
}

void RemoteHostRegistryTest::typedAnswerResolvesQueuedDuplicates()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString databasePath = dir.filePath(QStringLiteral("state.db"));
    prepareSchema(databasePath);

    SshAskpassBroker broker;
    QVERIFY(broker.listen());
    RemoteHostRegistry registry(databasePath, &broker);
    const QString id = registry.saveHost(hostFields(QStringLiteral("h")));
    const QVariantMap environment = registry.terminalEnvironment(id);
    QSignalSpy requested(&broker, &SshAskpassBroker::promptRequested);
    LaunchedSsh launched;

    // Terminal and file channel connect at once: one question for the user.
    auto terminal = ask(environment, launched.start(), QStringLiteral("alice@h's password: "));
    auto files = ask(environment, launched.start(), QStringLiteral("alice@h's password: "));
    QTRY_COMPARE_WITH_TIMEOUT(requested.size(), 2, 10000);
    QVERIFY(waitForPrompt(registry));
    registry.answerPrompt(registry.pendingPrompt().value(QStringLiteral("requestId")).toString(), true,
                          QStringLiteral("typed"), false);

    Exchange first;
    Exchange second;
    QVERIFY(settle(terminal, &first));
    QVERIFY(settle(files, &second));
    QCOMPARE(first.answer, QStringLiteral("typed"));
    QCOMPARE(second.answer, QStringLiteral("typed"));
    QVERIFY(registry.pendingPrompt().isEmpty());
    // Not remembered: nothing reaches the database.
    QVERIFY(!registry.hostDetails(id).value(QStringLiteral("hasPassword")).toBool());
    QVERIFY(storedPassword(databasePath).isEmpty());
}

void RemoteHostRegistryTest::trustedHostKeyIsAnsweredOnce()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString databasePath = dir.filePath(QStringLiteral("state.db"));
    prepareSchema(databasePath);

    SshAskpassBroker broker;
    QVERIFY(broker.listen());
    RemoteHostRegistry registry(databasePath, &broker);
    const QString id = registry.saveHost(hostFields(QStringLiteral("h")));
    const QVariantMap environment = registry.terminalEnvironment(id);
    const QString hostKey = QStringLiteral("The authenticity of host 'h' can't be established.\n"
                                           "ED25519 key fingerprint is SHA256:abc.\n"
                                           "Are you sure you want to continue connecting (yes/no/[fingerprint])? ");

    auto first = ask(environment, 40, hostKey);
    QVERIFY(waitForPrompt(registry));
    QCOMPARE(registry.pendingPrompt().value(QStringLiteral("kind")).toString(), QStringLiteral("hostKey"));
    registry.answerPrompt(registry.pendingPrompt().value(QStringLiteral("requestId")).toString(), true,
                          QStringLiteral("yes"), false);
    Exchange result;
    QVERIFY(settle(first, &result));
    QCOMPARE(result.answer, QStringLiteral("yes"));

    // The file channel meets the same key: no second question.
    auto second = ask(environment, 41, hostKey);
    QVERIFY(settle(second, &result));
    QCOMPARE(result.answer, QStringLiteral("yes"));
    QVERIFY(registry.pendingPrompt().isEmpty());

    // Refusing a different key fails that ssh.
    auto changed = ask(environment, 42, QString(hostKey).replace(QStringLiteral("abc"), QStringLiteral("xyz")));
    QVERIFY(waitForPrompt(registry));
    registry.answerPrompt(registry.pendingPrompt().value(QStringLiteral("requestId")).toString(), false, QString(),
                          false);
    QVERIFY(settle(changed, &result));
    QVERIFY(!result.ok);
}

void RemoteHostRegistryTest::terminalLaunchUsesAskpassAndPortableTerm()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString databasePath = dir.filePath(QStringLiteral("state.db"));
    prepareSchema(databasePath);

    SshAskpassBroker broker;
    QVERIFY(broker.listen());
    RemoteHostRegistry registry(databasePath, &broker);
    QVariantMap fields = hostFields(QStringLiteral("example.com"));
    fields.insert(QStringLiteral("port"), 2222);
    const QString id = registry.saveHost(fields);

    const QString commandLine = registry.terminalCommandLine(id);
    QVERIFY(commandLine.endsWith(QStringLiteral(" -p 2222 -l alice -- example.com")));
    const QVariantMap environment = registry.terminalEnvironment(id);
    QCOMPARE(environment.value(QStringLiteral("TERM")).toString(), QStringLiteral("xterm-256color"));
    QCOMPARE(environment.value(QStringLiteral("SSH_ASKPASS_REQUIRE")).toString(), QStringLiteral("force"));
    QCOMPARE(environment.value(QLatin1String(span::remote::kAskpassPipeEnv)).toString(), broker.pipeName());
    QVERIFY(registry.terminalCommandLine(QStringLiteral("missing")).isEmpty());
}

void RemoteHostRegistryTest::importsOwnerOnlyKeyCopy()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString databasePath = dir.filePath(QStringLiteral("state.db"));
    prepareSchema(databasePath);
    const QString original = dir.filePath(QStringLiteral("keys/id_test"));
    const QByteArray key("-----BEGIN OPENSSH PRIVATE KEY-----\nfixture\n");
    QVERIFY(writeFile(original, key));

    QString id;
    QString copyPath;
    QVariantMap fields = hostFields(QStringLiteral("example.com"));
    fields.insert(QStringLiteral("identityFile"), original);
    {
        RemoteHostRegistry registry(databasePath, nullptr);
        QSignalSpy errors(&registry, &RemoteHostRegistry::errorOccurred);
        id = registry.saveHost(fields);
        QVERIFY2(!id.isEmpty(), qPrintable(errors.isEmpty() ? QString() : errors.constFirst().constFirst().toString()));
        copyPath = dir.filePath(QStringLiteral("ssh-keys/") + id);
        QCOMPARE(readFile(copyPath), key);
        QVERIFY(isOwnerOnly(copyPath));
        QCOMPARE(readFile(original), key);
        QCOMPARE(registry.hostDetails(id).value(QStringLiteral("identityFile")).toString(), original);
        const QString commandLine = registry.terminalCommandLine(id);
        QVERIFY2(commandLine.contains(QDir::toNativeSeparators(copyPath)), qPrintable(commandLine));
        QVERIFY(!commandLine.contains(QDir::toNativeSeparators(original)));

        // The copy stands in for an original that disappeared.
        QVERIFY(QFile::remove(original));
        fields.insert(QStringLiteral("id"), id);
        fields.insert(QStringLiteral("name"), QStringLiteral("Renamed"));
        QCOMPARE(registry.validationError(fields), QString());
        QCOMPARE(registry.saveHost(fields), id);
        QCOMPARE(readFile(copyPath), key);

        fields.insert(QStringLiteral("identityFile"), QString());
        QCOMPARE(registry.saveHost(fields), id);
        QVERIFY(!QFileInfo::exists(copyPath));
        QVERIFY(!registry.terminalCommandLine(id).contains(QStringLiteral(" -i ")));

        QVERIFY(writeFile(original, key));
        fields.insert(QStringLiteral("identityFile"), original);
        QCOMPARE(registry.saveHost(fields), id);
        QVERIFY(QFileInfo::exists(copyPath));
    }

    // A host whose copy went missing is imported again on load.
    QVERIFY(QFile::remove(copyPath));
    RemoteHostRegistry reloaded(databasePath, nullptr);
    QCOMPARE(readFile(copyPath), key);
    QVERIFY(isOwnerOnly(copyPath));

    QVERIFY(reloaded.removeHost(id));
    QVERIFY(!QFileInfo::exists(copyPath));
    QCOMPARE(readFile(original), key);
}

void RemoteHostRegistryTest::failedStoreKeepsKeyCopy()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString databasePath = dir.filePath(QStringLiteral("state.db"));
    prepareSchema(databasePath);
    const QString original = dir.filePath(QStringLiteral("keys/id_old"));
    const QString replacement = dir.filePath(QStringLiteral("keys/id_new"));
    const QByteArray oldKey("-----BEGIN OPENSSH PRIVATE KEY-----\nold\n");
    const QByteArray newKey("-----BEGIN OPENSSH PRIVATE KEY-----\nnew\n");
    QVERIFY(writeFile(original, oldKey));
    QVERIFY(writeFile(replacement, newKey));

    RemoteHostRegistry registry(databasePath, nullptr);
    QVariantMap fields = hostFields(QStringLiteral("example.com"));
    fields.insert(QStringLiteral("identityFile"), original);
    const QString id = registry.saveHost(fields);
    QVERIFY(!id.isEmpty());
    const QString copyPath = dir.filePath(QStringLiteral("ssh-keys/") + id);
    const QString commandLine = registry.terminalCommandLine(id);
    fields.insert(QStringLiteral("id"), id);

    // Every write of the row fails from here on.
    const auto setLocked = [&](bool locked) {
        {
            QSqlDatabase db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), QStringLiteral("lock"));
            db.setDatabaseName(databasePath);
            QVERIFY(db.open());
            QSqlQuery query(db);
            QVERIFY(query.exec(locked ? QStringLiteral("create trigger lock_hosts before update on remote_hosts "
                                                       "begin select raise(abort, 'locked'); end")
                                      : QStringLiteral("drop trigger lock_hosts")));
        }
        QSqlDatabase::removeDatabase(QStringLiteral("lock"));
    };
    setLocked(true);

    // Switching keys: the stored record still names the old key, and so
    // does the copy ssh uses.
    fields.insert(QStringLiteral("identityFile"), replacement);
    QVERIFY(registry.saveHost(fields).isEmpty());
    QCOMPARE(readFile(copyPath), oldKey);
    QVERIFY(!QFileInfo::exists(copyPath + QStringLiteral(".staged")));

    // The original is gone and the copy is all that is left: a failed
    // "remove the key" save must not delete it.
    QVERIFY(QFile::remove(original));
    fields.insert(QStringLiteral("identityFile"), QString());
    QVERIFY(registry.saveHost(fields).isEmpty());
    QCOMPARE(readFile(copyPath), oldKey);
    QCOMPARE(registry.terminalCommandLine(id), commandLine);

    setLocked(false);
    fields.insert(QStringLiteral("identityFile"), replacement);
    QCOMPARE(registry.saveHost(fields), id);
    QCOMPARE(readFile(copyPath), newKey);
    QVERIFY(isOwnerOnly(copyPath));
    QVERIFY(!QFileInfo::exists(copyPath + QStringLiteral(".staged")));
}

void RemoteHostRegistryTest::failedKeySwapRollsBackSave()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString databasePath = dir.filePath(QStringLiteral("state.db"));
    prepareSchema(databasePath);
    const QString original = dir.filePath(QStringLiteral("keys/id_old"));
    const QString replacement = dir.filePath(QStringLiteral("keys/id_new"));
    const QByteArray oldKey("-----BEGIN OPENSSH PRIVATE KEY-----\nold\n");
    QVERIFY(writeFile(original, oldKey));
    QVERIFY(writeFile(replacement, QByteArray("-----BEGIN OPENSSH PRIVATE KEY-----\nnew\n")));

    QString id;
    QString copyPath;
    {
        RemoteHostRegistry registry(databasePath, nullptr);
        QVariantMap fields = hostFields(QStringLiteral("example.com"));
        fields.insert(QStringLiteral("identityFile"), original);
        id = registry.saveHost(fields);
        QVERIFY(!id.isEmpty());
        copyPath = dir.filePath(QStringLiteral("ssh-keys/") + id);

        // Something holds the copy open without delete sharing (an ssh
        // reading it, a scanner): it cannot be swapped out.
        const std::wstring native = QDir::toNativeSeparators(copyPath).toStdWString();
        HANDLE held = CreateFileW(native.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                                  FILE_ATTRIBUTE_NORMAL, nullptr);
        QVERIFY(held != INVALID_HANDLE_VALUE);
        QSignalSpy errors(&registry, &RemoteHostRegistry::errorOccurred);
        fields.insert(QStringLiteral("id"), id);
        fields.insert(QStringLiteral("host"), QStringLiteral("other.example"));
        fields.insert(QStringLiteral("identityFile"), replacement);
        const QString saved = registry.saveHost(fields);
        CloseHandle(held);
        QVERIFY(saved.isEmpty());
        QCOMPARE(errors.size(), 1);

        // Nothing changed: not the record in memory, not the copy.
        QCOMPARE(registry.hostDetails(id).value(QStringLiteral("host")).toString(), QStringLiteral("example.com"));
        QCOMPARE(registry.hostDetails(id).value(QStringLiteral("identityFile")).toString(), original);
        QCOMPARE(readFile(copyPath), oldKey);
        QVERIFY(!QFileInfo::exists(copyPath + QStringLiteral(".staged")));
        QVERIFY(!QFileInfo::exists(copyPath + QStringLiteral(".previous")));
    }

    // Nor the stored row.
    RemoteHostRegistry reloaded(databasePath, nullptr);
    QCOMPARE(reloaded.hostDetails(id).value(QStringLiteral("host")).toString(), QStringLiteral("example.com"));
    QCOMPARE(reloaded.hostDetails(id).value(QStringLiteral("identityFile")).toString(), original);
    QCOMPARE(readFile(copyPath), oldKey);
}

void RemoteHostRegistryTest::keyCopyInUseIsDeletedOnNextStart()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString databasePath = dir.filePath(QStringLiteral("state.db"));
    prepareSchema(databasePath);
    const QString original = dir.filePath(QStringLiteral("keys/id_test"));
    QVERIFY(writeFile(original, QByteArray("-----BEGIN OPENSSH PRIVATE KEY-----\nfixture\n")));

    QString copyPath;
    {
        RemoteHostRegistry registry(databasePath, nullptr);
        QVariantMap fields = hostFields(QStringLiteral("example.com"));
        fields.insert(QStringLiteral("identityFile"), original);
        const QString id = registry.saveHost(fields);
        QVERIFY(!id.isEmpty());
        copyPath = dir.filePath(QStringLiteral("ssh-keys/") + id);

        // A terminal's ssh still has the copy open when the host is removed.
        const std::wstring native = QDir::toNativeSeparators(copyPath).toStdWString();
        HANDLE held = CreateFileW(native.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                                  FILE_ATTRIBUTE_NORMAL, nullptr);
        QVERIFY(held != INVALID_HANDLE_VALUE);
        QTest::ignoreMessage(QtWarningMsg, QRegularExpression(QStringLiteral("^Cannot delete the SSH key copy now")));
        QVERIFY(registry.removeHost(id));
        CloseHandle(held);
        QVERIFY(QFileInfo::exists(copyPath));
    }

    RemoteHostRegistry restarted(databasePath, nullptr);
    QVERIFY(restarted.loaded());
    QVERIFY(!QFileInfo::exists(copyPath));
}

void RemoteHostRegistryTest::unreadableListPrunesNothing()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    // A database without the hosts table: the list cannot be read.
    const QString databasePath = dir.filePath(QStringLiteral("state.db"));
    const QString copyPath = dir.filePath(QStringLiteral("ssh-keys/some-host"));
    QVERIFY(writeFile(copyPath, QByteArray("key")));

    QTest::ignoreMessage(QtWarningMsg, QRegularExpression(QStringLiteral("^Cannot read the SSH host list:")));
    RemoteHostRegistry registry(databasePath, nullptr);
    QVERIFY(!registry.loaded());
    QCOMPARE(registry.count(), 0);
    // An empty list that was never read says nothing about this host.
    QVERIFY(QFileInfo::exists(copyPath));
}

void RemoteHostRegistryTest::keyPathFromPickedFile()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString databasePath = dir.filePath(QStringLiteral("state.db"));
    prepareSchema(databasePath);
    RemoteHostRegistry registry(databasePath, nullptr);

    QCOMPARE(registry.keyPathFromUrl(QUrl(QStringLiteral("file:///C:/Users/a%20b/.ssh/id_ed25519"))),
             QStringLiteral("C:\\Users\\a b\\.ssh\\id_ed25519"));
    // A network share keeps its server and share.
    QCOMPARE(registry.keyPathFromUrl(QUrl(QStringLiteral("file://server/share/id_ed25519"))),
             QStringLiteral("\\\\server\\share\\id_ed25519"));
    QVERIFY(registry.keyPathFromUrl(QUrl(QStringLiteral("https://example.com/key"))).isEmpty());

    // The local share of this machine stands in for a real one.
    const QString key = dir.filePath(QStringLiteral("id_share"));
    QVERIFY(writeFile(key, QByteArray("-----BEGIN OPENSSH PRIVATE KEY-----\nshare\n")));
    const QString native = QDir::toNativeSeparators(key);
    const QString unc = QStringLiteral("\\\\localhost\\") + native.left(1) + QStringLiteral("$") + native.mid(2);
    if (!QFileInfo(unc).isFile()) QSKIP("Administrative share is not reachable");
    const QUrl picked = QUrl::fromLocalFile(unc);
    QCOMPARE(picked.host(), QStringLiteral("localhost"));
    QVariantMap fields = hostFields(QStringLiteral("example.com"));
    fields.insert(QStringLiteral("identityFile"), registry.keyPathFromUrl(picked));
    QCOMPARE(registry.validationError(fields), QString());
}

void RemoteHostRegistryTest::emptyUserMeansRoot()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString databasePath = dir.filePath(QStringLiteral("state.db"));
    prepareSchema(databasePath);

    QVariantMap fields = hostFields(QStringLiteral("example.com"));
    fields.insert(QStringLiteral("user"), QString());
    QString id;
    {
        RemoteHostRegistry registry(databasePath, nullptr);
        id = registry.saveHost(fields);
        QVERIFY(!id.isEmpty());
        QCOMPARE(registry.hostDetails(id).value(QStringLiteral("user")).toString(), QStringLiteral("root"));
        QCOMPARE(registry.data(registry.index(0), RemoteHostRegistry::AddressRole).toString(),
                 QStringLiteral("root@example.com"));
        QVERIFY(registry.terminalCommandLine(id).endsWith(QStringLiteral(" -l root -- example.com")));
    }

    // Rows saved before the default existed still have an empty user.
    {
        QSqlDatabase db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), QStringLiteral("legacy"));
        db.setDatabaseName(databasePath);
        QVERIFY(db.open());
        QSqlQuery query(db);
        QVERIFY(query.exec(QStringLiteral("update remote_hosts set user=''")));
    }
    QSqlDatabase::removeDatabase(QStringLiteral("legacy"));
    RemoteHostRegistry reloaded(databasePath, nullptr);
    QCOMPARE(reloaded.hostDetails(id).value(QStringLiteral("user")).toString(), QStringLiteral("root"));
}

void RemoteHostRegistryTest::secretsGoOnlyToTheirOwnPrompts()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString databasePath = dir.filePath(QStringLiteral("state.db"));
    prepareSchema(databasePath);

    SshAskpassBroker broker;
    QVERIFY(broker.listen());
    RemoteHostRegistry registry(databasePath, &broker);
    QVariantMap fields = hostFields(QStringLiteral("h"), QStringLiteral("target-pw"));
    fields.insert(QStringLiteral("keyPassphrase"), QStringLiteral("key-pp"));
    const QString id = registry.saveHost(fields);
    const QVariantMap environment = registry.terminalEnvironment(id);
    LaunchedSsh launched;

    // A ProxyJump hop inherits the askpass environment, and on the same
    // address and user it asks in the very same words (the port is not in
    // the prompt). It is not an ssh the app started: it is asked, never
    // answered with the target's password, and its answer is not kept.
    Exchange result;
    auto hop = ask(environment, 40, QStringLiteral("alice@h's password: "));
    QVERIFY(answerPending(registry, QStringLiteral("password"), QStringLiteral("jump-pw")));
    QVERIFY(settle(hop, &result));
    QCOMPARE(result.answer, QStringLiteral("jump-pw"));

    // One-time codes are asked every time, never filled in or kept.
    auto otp = ask(environment, launched.start(), QStringLiteral("(alice@h) One-time password: "));
    QVERIFY(answerPending(registry, QStringLiteral("secret"), QStringLiteral("123456")));
    QVERIFY(settle(otp, &result));
    QCOMPARE(result.answer, QStringLiteral("123456"));

    // A server asking for "the passphrase" is a server challenge as well:
    // the key passphrase never reaches it.
    auto lure = ask(environment, launched.start(), QStringLiteral("(alice@h) Enter passphrase for key 'k': "));
    QVERIFY(answerPending(registry, QStringLiteral("secret"), QStringLiteral("typed")));
    QVERIFY(settle(lure, &result));

    // The target's own prompts still get the target's secrets, untouched by
    // the answers above: the password through either auth method, the
    // passphrase only for the local key.
    auto login = ask(environment, launched.start(), QStringLiteral("alice@h's password: "));
    QVERIFY(settle(login, &result));
    QCOMPARE(result.answer, QStringLiteral("target-pw"));
    auto challenge = ask(environment, launched.start(), QStringLiteral("(alice@h) Password: "));
    QVERIFY(settle(challenge, &result));
    QCOMPARE(result.answer, QStringLiteral("target-pw"));
    auto unlock = ask(environment, launched.start(), QStringLiteral("Enter passphrase for key 'k': "));
    QVERIFY(settle(unlock, &result));
    QCOMPARE(result.answer, QStringLiteral("key-pp"));
    QVERIFY(registry.pendingPrompt().isEmpty());
}

void RemoteHostRegistryTest::failedSaveKeepsSessionSecrets()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString databasePath = dir.filePath(QStringLiteral("state.db"));
    prepareSchema(databasePath);

    SshAskpassBroker broker;
    QVERIFY(broker.listen());
    RemoteHostRegistry registry(databasePath, &broker);
    const QString id = registry.saveHost(hostFields(QStringLiteral("a.example"), QStringLiteral("for-a")));
    const QVariantMap environment = registry.terminalEnvironment(id);

    // Moving the host to B fails at the key import (not a key file).
    const QString oversized = dir.filePath(QStringLiteral("huge.key"));
    QVERIFY(writeFile(oversized, QByteArray(128 * 1024, 'k')));
    QVariantMap edit = hostFields(QStringLiteral("b.example"), QStringLiteral("for-b"));
    edit.insert(QStringLiteral("id"), id);
    edit.insert(QStringLiteral("identityFile"), oversized);
    QSignalSpy errors(&registry, &RemoteHostRegistry::errorOccurred);
    QVERIFY(registry.saveHost(edit).isEmpty());
    QCOMPARE(errors.size(), 1);

    // The host still is A, with A's launch token and A's password.
    QCOMPARE(registry.hostDetails(id).value(QStringLiteral("host")).toString(), QStringLiteral("a.example"));
    QCOMPARE(registry.terminalEnvironment(id), environment);
    LaunchedSsh launched;
    Exchange result;
    auto login = ask(environment, launched.start(), QStringLiteral("alice@a.example's password: "));
    QVERIFY(settle(login, &result));
    QCOMPARE(result.answer, QStringLiteral("for-a"));
}

void RemoteHostRegistryTest::newDestinationRevokesOldLaunches()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString databasePath = dir.filePath(QStringLiteral("state.db"));
    prepareSchema(databasePath);

    SshAskpassBroker broker;
    QVERIFY(broker.listen());
    RemoteHostRegistry registry(databasePath, &broker);
    const QString id = registry.saveHost(hostFields(QStringLiteral("a.example"), QStringLiteral("for-a")));
    const QVariantMap launchedForA = registry.terminalEnvironment(id);
    const QString tokenKey = QLatin1String(span::remote::kAskpassTokenEnv);

    // Renaming keeps the destination, and so the processes already started.
    QVariantMap edit = hostFields(QStringLiteral("a.example"));
    edit.insert(QStringLiteral("id"), id);
    edit.insert(QStringLiteral("name"), QStringLiteral("Renamed"));
    QCOMPARE(registry.saveHost(edit), id);
    QCOMPARE(registry.terminalEnvironment(id).value(tokenKey), launchedForA.value(tokenKey));

    edit = hostFields(QStringLiteral("b.example"), QStringLiteral("for-b"));
    edit.insert(QStringLiteral("id"), id);
    QCOMPARE(registry.saveHost(edit), id);

    // A process still talking to A (queued transfer, open terminal) gets
    // nothing, least of all B's password.
    Exchange result;
    auto stale = ask(launchedForA, 30, QStringLiteral("alice@a.example's password: "));
    QVERIFY(settle(stale, &result));
    QVERIFY(!result.ok);
    QVERIFY(result.answer.isEmpty());
    QVERIFY(registry.pendingPrompt().isEmpty());

    const QVariantMap launchedForB = registry.terminalEnvironment(id);
    QVERIFY(launchedForB.value(tokenKey) != launchedForA.value(tokenKey));
    LaunchedSsh launched;
    auto fresh = ask(launchedForB, launched.start(), QStringLiteral("alice@b.example's password: "));
    QVERIFY(settle(fresh, &result));
    QVERIFY(result.ok);
    QCOMPARE(result.answer, QStringLiteral("for-b"));
}

void RemoteHostRegistryTest::reportsPromptBridgeState()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString databasePath = dir.filePath(QStringLiteral("state.db"));
    prepareSchema(databasePath);

    // A broker that never listened: ssh gets no askpass environment and the
    // Environment page reports the bridge as not running.
    SshAskpassBroker idle;
    RemoteHostRegistry withoutBridge(databasePath, &idle);
    QVERIFY(!withoutBridge.promptBridgeAvailable());
    QVERIFY(withoutBridge.promptBridgePipe().isEmpty());
    const QString id = withoutBridge.saveHost(hostFields(QStringLiteral("example.com"), QString()));
    QVERIFY(!id.isEmpty());
    QVERIFY(!withoutBridge.terminalEnvironment(id).contains(QString::fromLatin1(span::remote::kAskpassPipeEnv)));

    SshAskpassBroker broker;
    QVERIFY(broker.listen());
    QVERIFY(broker.listenError().isEmpty());
    RemoteHostRegistry withBridge(databasePath, &broker);
    QVERIFY(withBridge.promptBridgeAvailable());
    QVERIFY(withBridge.promptBridgeError().isEmpty());
    QCOMPARE(withBridge.promptBridgePipe(), broker.pipeName());
    QVERIFY(withBridge.promptBridgePipe().startsWith(QStringLiteral("\\\\.\\pipe\\")));
    QVERIFY(withBridge.terminalEnvironment(id).contains(QString::fromLatin1(span::remote::kAskpassPipeEnv)));
}

QTEST_MAIN(RemoteHostRegistryTest)

#include "remote_host_registry_test.moc"
