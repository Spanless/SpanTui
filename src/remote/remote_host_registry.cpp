#include "remote/remote_host_registry.h"

#include "core/core_constants.h"
#include "platform/windows/credential_protection.h"
#include "remote/remote_host_context.h"
#include "remote/ssh_askpass.h"

#include <QDebug>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QUuid>

#include <algorithm>

namespace span::remote {
namespace {

namespace credentials = span::platform::credentials;

QString promptKindName(int kind)
{
    switch (SshPromptKind(kind)) {
    case SshPromptKind::Password: return QStringLiteral("password");
    case SshPromptKind::Passphrase: return QStringLiteral("passphrase");
    case SshPromptKind::HostKey: return QStringLiteral("hostKey");
    case SshPromptKind::Secret: return QStringLiteral("secret");
    case SshPromptKind::Text: break;
    }
    return QStringLiteral("text");
}

bool isSecretKind(int kind)
{
    return kind == int(SshPromptKind::Password) || kind == int(SshPromptKind::Passphrase);
}

// An empty user would make ssh log in with the local Windows account name,
// which almost never exists on the server.
QString userOrDefault(const QString &user)
{
    return user.isEmpty() ? QStringLiteral("root") : user;
}

SshEndpoint endpointFromFields(const QVariantMap &fields)
{
    SshEndpoint endpoint;
    endpoint.host = fields.value(QStringLiteral("host")).toString().trimmed();
    endpoint.port = fields.value(QStringLiteral("port"), 22).toInt();
    endpoint.user = userOrDefault(fields.value(QStringLiteral("user")).toString().trimmed());
    endpoint.identityFile = fields.value(QStringLiteral("identityFile")).toString().trimmed();
    return endpoint;
}

// SQLite binds a null QString as NULL; the text columns are NOT NULL.
QString columnText(const QString &value)
{
    return value.isNull() ? QStringLiteral("") : value;
}

// Real private keys are a few KiB; anything larger is not a key file.
constexpr qint64 kMaxKeyFileSize = 64 * 1024;

QString addressText(const SshEndpoint &endpoint)
{
    QString address = endpoint.user.isEmpty() ? endpoint.host : endpoint.user + QLatin1Char('@') + endpoint.host;
    if (endpoint.port != 22) {
        address += QLatin1Char(':') + QString::number(endpoint.port);
    }
    return address;
}

QByteArray fileBytes(const QString &path)
{
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
}

// Host, port and user decide which server receives the saved secrets.
bool sameDestination(const SshEndpoint &a, const SshEndpoint &b)
{
    return a.host == b.host && a.port == b.port && a.user == b.user;
}

} // namespace

RemoteHostRegistry::RemoteHostRegistry(QString databasePath, SshAskpassBroker *broker, QObject *parent)
    : QAbstractListModel(parent),
      m_databasePath(std::move(databasePath)),
      m_connectionName(QStringLiteral("remote-hosts-%1").arg(QUuid::createUuid().toString(QUuid::Id128))),
      m_broker(broker)
{
    m_loaded = load();
    if (m_loaded) {
        removeOrphanKeyCopies();
    }
    if (m_broker) {
        connect(m_broker, &SshAskpassBroker::promptRequested, this, &RemoteHostRegistry::handlePrompt);
        connect(m_broker, &SshAskpassBroker::promptAbandoned, this, &RemoteHostRegistry::dropPrompt);
    }
}

RemoteHostRegistry::~RemoteHostRegistry()
{
    {
        QSqlDatabase db = QSqlDatabase::database(m_connectionName, false);
        if (db.isValid()) {
            db.close();
        }
    }
    QSqlDatabase::removeDatabase(m_connectionName);
}

int RemoteHostRegistry::rowCount(const QModelIndex &parent) const
{
    return parent.isValid() ? 0 : int(m_hosts.size());
}

QVariant RemoteHostRegistry::data(const QModelIndex &index, int role) const
{
    if (!index.isValid() || index.row() < 0 || index.row() >= m_hosts.size()) {
        return {};
    }
    const Host &host = m_hosts.at(index.row());
    switch (role) {
    case IdRole: return host.id;
    case NameRole: return host.name;
    case AddressRole: return addressText(host.endpoint);
    case ExpandedRole: return host.expanded;
    default: return {};
    }
}

QHash<int, QByteArray> RemoteHostRegistry::roleNames() const
{
    return {
        {IdRole, "hostId"},
        {NameRole, "name"},
        {AddressRole, "address"},
        {ExpandedRole, "expanded"},
    };
}

QStringList RemoteHostRegistry::hostIds() const
{
    QStringList ids;
    ids.reserve(m_hosts.size());
    for (const Host &host : m_hosts) {
        ids << host.id;
    }
    return ids;
}

bool RemoteHostRegistry::contains(const QString &hostId) const
{
    return indexForId(hostId) >= 0;
}

QString RemoteHostRegistry::hostName(const QString &hostId) const
{
    const int row = indexForId(hostId);
    return row < 0 ? QString() : m_hosts.at(row).name;
}

QString RemoteHostRegistry::hostIdForAddress(const QStringList &addresses) const
{
    for (const Host &host : m_hosts) {
        for (const QString &address : addresses) {
            if (!address.isEmpty() && host.endpoint.host.compare(address, Qt::CaseInsensitive) == 0) {
                return host.id;
            }
        }
    }
    return {};
}

bool RemoteHostRegistry::endpointFor(const QString &hostId, SshEndpoint *endpoint) const
{
    const int row = indexForId(hostId);
    if (row < 0) {
        return false;
    }
    if (endpoint) {
        *endpoint = m_hosts.at(row).endpoint;
    }
    return true;
}

QVariantMap RemoteHostRegistry::hostDetails(const QString &hostId) const
{
    const int row = indexForId(hostId);
    if (row < 0) {
        return {};
    }
    const Host &host = m_hosts.at(row);
    return {
        {QStringLiteral("id"), host.id},
        {QStringLiteral("name"), host.name},
        {QStringLiteral("host"), host.endpoint.host},
        {QStringLiteral("port"), host.endpoint.port},
        {QStringLiteral("user"), host.endpoint.user},
        {QStringLiteral("identityFile"), host.endpoint.identityFile},
        {QStringLiteral("hasPassword"), !host.password.isEmpty()},
        {QStringLiteral("hasKeyPassphrase"), !host.keyPassphrase.isEmpty()},
    };
}

QString RemoteHostRegistry::validationError(const QVariantMap &fields) const
{
    SshEndpoint endpoint = endpointFromFields(fields);
    const QString id = fields.value(QStringLiteral("id")).toString();
    const int row = indexForId(id);
    // The private copy stands in for an original that was moved or deleted
    // after it was imported.
    if (row >= 0 && endpoint.identityFile == m_hosts.at(row).endpoint.identityFile
        && QFileInfo(keyCopyPath(id)).isFile()) {
        endpoint.identityFile.clear();
    }
    switch (checkEndpoint(endpoint)) {
    case EndpointError::None: return {};
    case EndpointError::Host: return tr("Enter a host name or IP address without spaces.");
    case EndpointError::Port: return tr("The port must be between 1 and 65535.");
    case EndpointError::User: return tr("The user name contains unsupported characters.");
    case EndpointError::IdentityFile: return tr("The private key file does not exist.");
    }
    return {};
}

bool RemoteHostRegistry::movesHost(const QVariantMap &fields) const
{
    const int row = indexForId(fields.value(QStringLiteral("id")).toString());
    return row >= 0 && !sameDestination(m_hosts.at(row).endpoint, endpointFromFields(fields));
}

QString RemoteHostRegistry::keyPathFromUrl(const QUrl &url) const
{
    return url.isLocalFile() ? QDir::toNativeSeparators(url.toLocalFile()) : QString();
}

QString RemoteHostRegistry::saveHost(const QVariantMap &fields)
{
    const QString error = validationError(fields);
    if (!error.isEmpty()) {
        emit errorOccurred(error);
        return {};
    }
    const QString id = fields.value(QStringLiteral("id")).toString();
    const int row = indexForId(id);
    Host host;
    if (row >= 0) {
        host = m_hosts.at(row);
    } else {
        host.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
        int maxOrder = -1;
        for (const Host &existing : std::as_const(m_hosts)) {
            maxOrder = std::max(maxOrder, existing.sortOrder);
        }
        host.sortOrder = maxOrder + 1;
    }
    host.endpoint = endpointFromFields(fields);
    host.name = fields.value(QStringLiteral("name")).toString().trimmed();
    if (host.name.isEmpty()) {
        host.name = host.endpoint.host;
    }
    const bool moved = movesHost(fields);

    // Typed secrets reach the session cache only once the record is saved:
    // until then the host still connects to its old endpoint.
    QHash<int, QString> typedSecrets; // kind -> new secret, empty = cleared
    const auto applySecret = [&](QByteArray *blob, const QString &valueKey, const QString &clearKey, int kind) {
        const QString value = fields.value(valueKey).toString();
        if (!value.isEmpty()) {
            *blob = credentials::protectSecret(value);
            typedSecrets.insert(kind, value);
        } else if (fields.value(clearKey).toBool()) {
            blob->clear();
            typedSecrets.insert(kind, QString());
        }
    };
    applySecret(&host.password, QStringLiteral("password"), QStringLiteral("clearPassword"),
                int(SshPromptKind::Password));
    applySecret(&host.keyPassphrase, QStringLiteral("keyPassphrase"), QStringLiteral("clearKeyPassphrase"),
                int(SshPromptKind::Passphrase));
    // A saved login password belongs to the server it was saved for: moving
    // the host (address, port or user) drops it unless a new one is typed.
    // The key passphrase only unlocks the local key and stays.
    if (moved && !typedSecrets.contains(int(SshPromptKind::Password))) {
        host.password.clear();
        typedSecrets.insert(int(SshPromptKind::Password), QString());
    }

    // Row and key copy change atomically: the old copy waits as ".previous"
    // until the transaction commits and is restored on failure. An unchanged
    // key whose original is gone keeps its copy.
    const QString copyPath = keyCopyPath(host.id);
    const QString stagedPath = copyPath + QStringLiteral(".staged");
    const QString previousPath = copyPath + QStringLiteral(".previous");
    const bool clearKey = host.endpoint.identityFile.isEmpty();
    const bool replaceKey = !clearKey && QFileInfo(host.endpoint.identityFile).isFile();
    const bool setAside = (clearKey || replaceKey) && QFileInfo(copyPath).isFile();
    QString keyError;
    if (replaceKey && !importKey(host, stagedPath, &keyError)) {
        emit errorOccurred(keyError);
        return {};
    }
    // The copy keeps its path whatever key it holds, so a new key is only
    // visible in its bytes; the file browser reconnects with it.
    const bool keyChanged = replaceKey && (!setAside || fileBytes(stagedPath) != fileBytes(copyPath));
    QFile::remove(previousPath);
    QSqlDatabase db = QSqlDatabase::database(m_connectionName);
    if (!db.transaction()) {
        QFile::remove(stagedPath);
        emit errorOccurred(db.lastError().text());
        return {};
    }
    // store() reports its own error.
    bool saved = store(host);
    if (saved && setAside) {
        saved = credentials::replaceFile(copyPath, previousPath, &keyError);
    }
    if (saved && replaceKey) {
        saved = credentials::replaceFile(stagedPath, copyPath, &keyError);
    }
    if (saved && !db.commit()) {
        saved = false;
        emit errorOccurred(db.lastError().text());
    }
    if (!saved) {
        db.rollback();
        QFile::remove(stagedPath);
        if (setAside && QFileInfo(previousPath).isFile()) {
            QString ignored;
            credentials::replaceFile(previousPath, copyPath, &ignored);
        } else if (!setAside && replaceKey) {
            QFile::remove(copyPath);
        }
        if (!keyError.isEmpty()) {
            emit errorOccurred(tr("Could not store a private copy of the key: %1").arg(keyError));
        }
        return {};
    }
    QFile::remove(previousPath);
    // Processes started for the old server keep the old token; revoking it
    // stops secrets crossing between the two servers.
    if (moved) {
        retireSession(host.id);
    }
    for (auto it = typedSecrets.cbegin(); it != typedSecrets.cend(); ++it) {
        if (it.value().isEmpty()) {
            m_sessionSecrets[host.id].remove(it.key());
        } else {
            m_sessionSecrets[host.id].insert(it.key(), it.value());
        }
    }
    if (row >= 0) {
        m_hosts[row] = host;
        emit dataChanged(index(row), index(row));
        if (auto context = m_contexts.value(host.id)) {
            context->reconfigure(launchEndpoint(host), launchEnvironment(host.id), keyChanged);
        }
    } else {
        beginInsertRows({}, int(m_hosts.size()), int(m_hosts.size()));
        m_hosts.push_back(host);
        endInsertRows();
    }
    emit hostsChanged();
    return host.id;
}

bool RemoteHostRegistry::removeHost(const QString &hostId)
{
    const int row = indexForId(hostId);
    if (row < 0) {
        return false;
    }
    QSqlQuery query(QSqlDatabase::database(m_connectionName));
    query.prepare(QStringLiteral("delete from remote_hosts where id=?"));
    query.addBindValue(hostId);
    if (!query.exec()) {
        emit errorOccurred(query.lastError().text());
        return false;
    }
    if (auto context = m_contexts.take(hostId)) {
        context->deleteLater();
    }
    // A copy still open (an ssh of the host's terminals) is deleted on the
    // next start by removeOrphanKeyCopies().
    if (QFileInfo::exists(keyCopyPath(hostId)) && !QFile::remove(keyCopyPath(hostId))) {
        qWarning().noquote() << "Cannot delete the SSH key copy now; it is removed on the next start:"
                             << QDir::toNativeSeparators(keyCopyPath(hostId));
    }
    retireSession(hostId);
    beginRemoveRows({}, row, row);
    m_hosts.removeAt(row);
    endRemoveRows();
    emit hostsChanged();
    return true;
}

bool RemoteHostRegistry::setExpanded(const QString &hostId, bool expanded)
{
    const int row = indexForId(hostId);
    if (row < 0 || m_hosts.at(row).expanded == expanded) {
        return row >= 0;
    }
    Host updated = m_hosts.at(row);
    updated.expanded = expanded;
    if (!store(updated)) {
        return false;
    }
    m_hosts[row] = updated;
    emit dataChanged(index(row), index(row), {ExpandedRole});
    return true;
}

RemoteHostContext *RemoteHostRegistry::contextForHost(const QString &hostId)
{
    const int row = indexForId(hostId);
    if (row < 0) {
        return nullptr;
    }
    if (auto context = m_contexts.value(hostId)) {
        return context;
    }
    auto *context = new RemoteHostContext(hostId, launchEndpoint(m_hosts.at(row)), launchEnvironment(hostId), this);
    m_contexts.insert(hostId, context);
    emit contextCreated(context);
    return context;
}

QString RemoteHostRegistry::terminalCommandLine(const QString &hostId) const
{
    const int row = indexForId(hostId);
    if (row < 0) {
        return {};
    }
    // A missing client still yields an ssh command line: the session then
    // fails to start visibly instead of silently opening a local shell.
    QString program = sshExecutable();
    if (program.isEmpty()) {
        program = QStringLiteral("ssh.exe");
    }
    return windowsCommandLine(program, sshTerminalArguments(launchEndpoint(m_hosts.at(row))));
}

QVariantMap RemoteHostRegistry::terminalEnvironment(const QString &hostId)
{
    QVariantMap environment;
    // No token for a host that is gone: its terminals are about to close.
    if (!contains(hostId)) {
        return environment;
    }
    const QHash<QString, QString> launch = launchEnvironment(hostId);
    for (auto it = launch.cbegin(); it != launch.cend(); ++it) {
        environment.insert(it.key(), it.value());
    }
    // ssh forwards TERM to the server, where xterm-kitty terminfo is rarely
    // installed; the remote side gets the universally known name.
    environment.insert(QStringLiteral("TERM"), QStringLiteral("xterm-256color"));
    return environment;
}

QVariantMap RemoteHostRegistry::pendingPrompt() const
{
    if (m_prompts.isEmpty()) {
        return {};
    }
    const Prompt &prompt = m_prompts.constFirst();
    const int row = indexForId(prompt.hostId);
    return {
        {QStringLiteral("requestId"), QString::number(prompt.requestId)},
        {QStringLiteral("hostName"), row < 0 ? QString() : m_hosts.at(row).name},
        {QStringLiteral("address"), row < 0 ? QString() : addressText(m_hosts.at(row).endpoint)},
        {QStringLiteral("kind"), promptKindName(prompt.kind)},
        {QStringLiteral("text"), prompt.text.trimmed()},
        {QStringLiteral("retry"), prompt.attempt > 1},
        {QStringLiteral("canRemember"), prompt.forHost},
    };
}

void RemoteHostRegistry::answerPrompt(const QString &requestId, bool accepted, const QString &answer, bool remember)
{
    const quint64 id = requestId.toULongLong();
    const auto it = std::find_if(m_prompts.cbegin(), m_prompts.cend(),
                                 [id](const Prompt &prompt) { return prompt.requestId == id; });
    if (it == m_prompts.cend()) {
        return;
    }
    const Prompt prompt = *it;
    dropPrompt(id);
    if (m_broker) {
        m_broker->respond(id, accepted, answer);
    }
    if (!accepted) {
        return;
    }
    if (isSecretKind(prompt.kind)) {
        if (!prompt.forHost) {
            return;
        }
        m_sessionSecrets[prompt.hostId].insert(prompt.kind, answer);
        const int row = indexForId(prompt.hostId);
        if (remember && row >= 0) {
            Host updated = m_hosts.at(row);
            QByteArray &blob = prompt.kind == int(SshPromptKind::Password) ? updated.password : updated.keyPassphrase;
            blob = credentials::protectSecret(answer);
            if (store(updated)) {
                m_hosts[row] = updated;
            }
        }
    } else if (prompt.kind == int(SshPromptKind::HostKey)) {
        m_acceptedHostKeys[prompt.hostId] << prompt.text;
    }
    resolveQueued(prompt.hostId, prompt.kind, prompt.text, answer);
}

int RemoteHostRegistry::indexForId(const QString &hostId) const
{
    if (hostId.isEmpty()) {
        return -1;
    }
    for (int i = 0; i < m_hosts.size(); ++i) {
        if (m_hosts.at(i).id == hostId) {
            return i;
        }
    }
    return -1;
}

bool RemoteHostRegistry::load()
{
    // Runs from the constructor, before anyone listens to errorOccurred: the
    // warnings reach the message center instead.
    QSqlDatabase db = QSqlDatabase::addDatabase(span::core::kSqliteDriver, m_connectionName);
    db.setDatabaseName(m_databasePath);
    if (!db.open()) {
        qWarning().noquote() << "Cannot open the SSH host list:" << db.lastError().text();
        return false;
    }
    QSqlQuery query(db);
    if (!query.exec(QStringLiteral("select id,name,host,port,user,identity_file,password,key_passphrase,"
                                   "expanded,sort_order from remote_hosts order by sort_order asc,name asc"))) {
        qWarning().noquote() << "Cannot read the SSH host list:" << query.lastError().text();
        return false;
    }
    while (query.next()) {
        Host host;
        host.id = query.value(0).toString();
        host.name = query.value(1).toString();
        host.endpoint.host = query.value(2).toString();
        host.endpoint.port = query.value(3).toInt();
        host.endpoint.user = userOrDefault(query.value(4).toString());
        host.endpoint.identityFile = query.value(5).toString();
        host.password = query.value(6).toByteArray();
        host.keyPassphrase = query.value(7).toByteArray();
        host.expanded = query.value(8).toBool();
        host.sortOrder = query.value(9).toInt();
        // Hosts saved before keys were imported, or whose copy was lost.
        if (!host.endpoint.identityFile.isEmpty() && !QFileInfo(keyCopyPath(host.id)).isFile()
            && QFileInfo(host.endpoint.identityFile).isFile()) {
            QString keyError;
            if (!importKey(host, keyCopyPath(host.id), &keyError)) {
                qWarning().noquote() << keyError;
            }
        }
        m_hosts.push_back(host);
    }
    return true;
}

void RemoteHostRegistry::removeOrphanKeyCopies() const
{
    QDir keys(QFileInfo(keyCopyPath(QStringLiteral("x"))).absolutePath());
    const QStringList files = keys.entryList(QDir::Files | QDir::Hidden);
    for (const QString &name : files) {
        if (indexForId(name) >= 0) {
            continue;
        }
        if (!keys.remove(name)) {
            qWarning().noquote() << "Cannot delete the unused SSH key copy" << keys.filePath(name);
        }
    }
}

QString RemoteHostRegistry::keyCopyPath(const QString &hostId) const
{
    return QFileInfo(m_databasePath).absolutePath() + QStringLiteral("/ssh-keys/") + hostId;
}

bool RemoteHostRegistry::importKey(const Host &host, const QString &copyPath, QString *error) const
{
    QFile original(host.endpoint.identityFile);
    if (original.size() > kMaxKeyFileSize) {
        *error = tr("The private key file is too large.");
        return false;
    }
    if (!original.open(QIODevice::ReadOnly)) {
        *error = tr("Could not store a private copy of the key: %1").arg(original.errorString());
        return false;
    }
    QByteArray key = original.read(kMaxKeyFileSize + 1);
    original.close();
    QString reason;
    const bool stored = credentials::makeOwnerOnlyDirectory(QFileInfo(copyPath).absolutePath(), &reason)
        && credentials::writeOwnerOnlyFile(copyPath, key, &reason);
    key.fill('\0');
    if (!stored) {
        *error = tr("Could not store a private copy of the key: %1").arg(reason);
    }
    return stored;
}

SshEndpoint RemoteHostRegistry::launchEndpoint(const Host &host) const
{
    SshEndpoint endpoint = host.endpoint;
    if (!endpoint.identityFile.isEmpty()) {
        const QString copyPath = keyCopyPath(host.id);
        if (QFileInfo(copyPath).isFile()) {
            endpoint.identityFile = copyPath;
        }
    }
    return endpoint;
}

bool RemoteHostRegistry::store(const Host &host)
{
    QSqlQuery query(QSqlDatabase::database(m_connectionName));
    query.prepare(QStringLiteral("insert into remote_hosts(id,name,host,port,user,identity_file,password,"
                                 "key_passphrase,expanded,sort_order) values(?,?,?,?,?,?,?,?,?,?) "
                                 "on conflict(id) do update set name=excluded.name,host=excluded.host,"
                                 "port=excluded.port,user=excluded.user,identity_file=excluded.identity_file,"
                                 "password=excluded.password,key_passphrase=excluded.key_passphrase,"
                                 "expanded=excluded.expanded,sort_order=excluded.sort_order"));
    query.addBindValue(host.id);
    query.addBindValue(columnText(host.name));
    query.addBindValue(columnText(host.endpoint.host));
    query.addBindValue(host.endpoint.port);
    query.addBindValue(columnText(host.endpoint.user));
    query.addBindValue(columnText(host.endpoint.identityFile));
    query.addBindValue(host.password);
    query.addBindValue(host.keyPassphrase);
    query.addBindValue(host.expanded ? 1 : 0);
    query.addBindValue(host.sortOrder);
    if (!query.exec()) {
        emit errorOccurred(query.lastError().text());
        return false;
    }
    return true;
}

void RemoteHostRegistry::retireSession(const QString &hostId)
{
    if (m_broker && m_tokens.contains(hostId)) {
        m_broker->revokeToken(m_tokens.take(hostId));
    }
    m_sessionSecrets.remove(hostId);
    m_acceptedHostKeys.remove(hostId);
    const QVector<Prompt> prompts = m_prompts;
    for (const Prompt &prompt : prompts) {
        if (prompt.hostId == hostId) {
            if (m_broker) {
                m_broker->respond(prompt.requestId, false, {});
            }
            dropPrompt(prompt.requestId);
        }
    }
}

QString RemoteHostRegistry::tokenFor(const QString &hostId)
{
    auto it = m_tokens.find(hostId);
    if (it == m_tokens.end()) {
        it = m_tokens.insert(hostId, m_broker->issueToken(hostId));
    }
    return it.value();
}

bool RemoteHostRegistry::promptBridgeAvailable() const
{
    return m_broker && !m_broker->pipeName().isEmpty();
}

QString RemoteHostRegistry::promptBridgeError() const
{
    return m_broker ? m_broker->listenError() : QString();
}

QString RemoteHostRegistry::promptBridgePipe() const
{
    return m_broker ? m_broker->pipeName() : QString();
}

QHash<QString, QString> RemoteHostRegistry::launchEnvironment(const QString &hostId)
{
    // Without a listening broker ssh keeps its own prompts: the terminal
    // still works, the file channel then needs key or agent authentication.
    if (!promptBridgeAvailable()) {
        return {};
    }
    return m_broker->environment(tokenFor(hostId));
}

QString RemoteHostRegistry::knownSecret(const QString &hostId, int kind) const
{
    const QString typed = m_sessionSecrets.value(hostId).value(kind);
    if (!typed.isEmpty()) {
        return typed;
    }
    const int row = indexForId(hostId);
    if (row < 0) {
        return {};
    }
    const QByteArray &blob =
        kind == int(SshPromptKind::Password) ? m_hosts.at(row).password : m_hosts.at(row).keyPassphrase;
    return credentials::unprotectSecret(blob);
}

bool RemoteHostRegistry::promptIsForHost(const Host &host, const QString &text, int kind, bool launched) const
{
    // Only an ssh this app started authenticates to the host itself; a
    // ProxyJump hop is started by that ssh.
    if (!launched) {
        return false;
    }
    if (kind == int(SshPromptKind::Password)) {
        return true;
    }
    if (kind != int(SshPromptKind::Passphrase)) {
        return false;
    }
    // ssh names the key it unlocks (cut to 100 characters); a hop's own key
    // is another file.
    QString key = launchEndpoint(host).identityFile;
    if (key.isEmpty()) {
        return true;
    }
    const QString lead = QStringLiteral("Enter passphrase for key '");
    QString shown = text.mid(lead.size());
    shown.truncate(shown.lastIndexOf(QLatin1Char('\'')));
    shown.replace(QLatin1Char('\\'), QLatin1Char('/'));
    key.replace(QLatin1Char('\\'), QLatin1Char('/'));
    return !shown.isEmpty() && key.startsWith(shown, Qt::CaseInsensitive);
}

void RemoteHostRegistry::handlePrompt(quint64 requestId, const QString &hostId, const QString &text, int kind,
                                      int attempt, bool launched)
{
    const int row = indexForId(hostId);
    if (row < 0) {
        m_broker->respond(requestId, false, {});
        return;
    }
    // A secret prompt that cannot be tied to this host's own login (a
    // ProxyJump hop, another key) is asked as is: nothing saved is offered,
    // and the answer is neither remembered nor reused.
    const bool forHost = promptIsForHost(m_hosts.at(row), text, kind, launched);
    if (forHost) {
        if (attempt == 1) {
            const QString secret = knownSecret(hostId, kind);
            if (!secret.isEmpty()) {
                m_broker->respond(requestId, true, secret);
                return;
            }
        } else {
            // The remembered secret was just rejected; ask instead of
            // offering it again on the next connection.
            m_sessionSecrets[hostId].remove(kind);
        }
    } else if (kind == int(SshPromptKind::HostKey) && m_acceptedHostKeys.value(hostId).contains(text)) {
        m_broker->respond(requestId, true, QStringLiteral("yes"));
        return;
    }
    m_prompts.push_back(Prompt{requestId, hostId, text, kind, attempt, forHost});
    if (m_prompts.size() == 1) {
        emit pendingPromptChanged();
    }
}

void RemoteHostRegistry::dropPrompt(quint64 requestId)
{
    const auto it = std::find_if(m_prompts.begin(), m_prompts.end(),
                                 [requestId](const Prompt &prompt) { return prompt.requestId == requestId; });
    if (it == m_prompts.end()) {
        return;
    }
    const bool wasCurrent = it == m_prompts.begin();
    m_prompts.erase(it);
    if (wasCurrent) {
        emit pendingPromptChanged();
    }
}

void RemoteHostRegistry::resolveQueued(const QString &hostId, int kind, const QString &text, const QString &answer)
{
    const QVector<Prompt> prompts = m_prompts;
    for (const Prompt &queued : prompts) {
        if (queued.hostId != hostId || queued.kind != kind || queued.attempt != 1
            || (isSecretKind(kind) && !queued.forHost)) {
            continue;
        }
        const bool sameHostKey = kind == int(SshPromptKind::HostKey) && queued.text == text;
        if (!isSecretKind(kind) && !sameHostKey) {
            continue;
        }
        dropPrompt(queued.requestId);
        m_broker->respond(queued.requestId, true, sameHostKey ? QStringLiteral("yes") : answer);
    }
}

} // namespace span::remote
