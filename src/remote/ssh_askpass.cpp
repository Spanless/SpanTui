#include "remote/ssh_askpass.h"

#include <QCoreApplication>
#include <QDir>
#include <QLocalServer>
#include <QLocalSocket>
#include <QRandomGenerator>

#include <qt_windows.h>
#include <shellapi.h>
#include <tlhelp32.h>

#include <iterator>
#include <string>

namespace span::remote {
namespace {

constexpr char kMagic[] = "SPANTUI-ASKPASS 1";
constexpr qsizetype kMaxLine = 64 * 1024;
const char *const kRequestIdProperty = "spantuiAskpassRequest";

QString randomHex(int bytes)
{
    QByteArray raw(bytes, Qt::Uninitialized);
    QRandomGenerator::system()->fillRange(
        reinterpret_cast<quint32 *>(raw.data()), bytes / int(sizeof(quint32)));
    return QString::fromLatin1(raw.toHex());
}

quint32 parentProcessId()
{
    const DWORD self = GetCurrentProcessId();
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) {
        return 0;
    }
    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    quint32 parent = 0;
    for (BOOL more = Process32FirstW(snapshot, &entry); more;
         more = Process32NextW(snapshot, &entry)) {
        if (entry.th32ProcessID == self) {
            parent = entry.th32ParentProcessID;
            break;
        }
    }
    CloseHandle(snapshot);
    return parent;
}

bool writeAll(HANDLE handle, const QByteArray &bytes)
{
    qsizetype offset = 0;
    while (offset < bytes.size()) {
        DWORD written = 0;
        if (!WriteFile(handle, bytes.constData() + offset,
                       DWORD(bytes.size() - offset), &written, nullptr)
            || written == 0) {
            return false;
        }
        offset += written;
    }
    return true;
}

} // namespace

SshPromptKind classifySshPrompt(const QString &prompt)
{
    const QString lower = prompt.toLower();
    // ssh's own host-key question; server text arrives behind "(user@host) ".
    if (!prompt.startsWith(QLatin1Char('(')) && lower.contains(QStringLiteral("(yes/no"))) {
        return SshPromptKind::HostKey;
    }
    // Only ssh's own wording unlocks a local key. Keyboard-interactive text
    // comes from the server and may say "passphrase" as well; answering it
    // with the key passphrase would hand that secret to the server.
    if (prompt.startsWith(QStringLiteral("Enter passphrase for key '"))) {
        return SshPromptKind::Passphrase;
    }
    // The login password: ssh's password authentication ("user@host's
    // password: ") or a bare keyboard-interactive "Password:", which ssh
    // shows behind a "(user@host) " prefix. One-time passwords and other
    // challenges that merely mention a password are never reused.
    if (!prompt.startsWith(QLatin1Char('(')) && prompt.endsWith(QStringLiteral("'s password: "))) {
        return SshPromptKind::Password;
    }
    QString challenge = prompt;
    if (challenge.startsWith(QLatin1Char('('))) {
        const qsizetype close = challenge.indexOf(QStringLiteral(") "));
        challenge = close < 0 ? QString() : challenge.mid(close + 2);
    }
    if (challenge.trimmed().compare(QStringLiteral("password:"), Qt::CaseInsensitive) == 0) {
        return SshPromptKind::Password;
    }
    if (lower.contains(QStringLiteral("password")) || lower.contains(QStringLiteral("passphrase"))) {
        return SshPromptKind::Secret;
    }
    return SshPromptKind::Text;
}

bool isLaunchedSsh(quint32 sshPid, quint32 ownerPid)
{
    if (sshPid == 0 || ownerPid == 0) {
        return false;
    }
    struct Process {
        quint32 parent = 0;
        QString exe;
    };
    QHash<quint32, Process> processes;
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) {
        return false;
    }
    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    for (BOOL more = Process32FirstW(snapshot, &entry); more;
         more = Process32NextW(snapshot, &entry)) {
        processes.insert(entry.th32ProcessID,
                         Process{entry.th32ParentProcessID,
                                 QString::fromWCharArray(entry.szExeFile)});
    }
    CloseHandle(snapshot);
    const auto ssh = processes.constFind(sshPid);
    if (ssh == processes.cend()) {
        return false;
    }
    if (ssh->parent == ownerPid) {
        return true;
    }
    // sftp runs its own ssh child for each transfer.
    const auto launcher = processes.constFind(ssh->parent);
    return launcher != processes.cend() && launcher->parent == ownerPid
        && launcher->exe.compare(QStringLiteral("sftp.exe"), Qt::CaseInsensitive) == 0;
}

bool isAskpassInvocation(int argc)
{
    return argc == 2 && qEnvironmentVariableIsSet(kAskpassPipeEnv)
        && qEnvironmentVariableIsSet(kAskpassTokenEnv);
}

bool exchangeAskpassPrompt(const QString &pipeName, const QString &token,
                           quint32 parentProcessId, const QString &prompt,
                           QString *answer)
{
    const std::wstring name = pipeName.toStdWString();
    HANDLE pipe = INVALID_HANDLE_VALUE;
    for (int attempt = 0; attempt < 20; ++attempt) {
        pipe = CreateFileW(name.c_str(), GENERIC_READ | GENERIC_WRITE, 0,
                           nullptr, OPEN_EXISTING, 0, nullptr);
        if (pipe != INVALID_HANDLE_VALUE || GetLastError() != ERROR_PIPE_BUSY) {
            break;
        }
        WaitNamedPipeW(name.c_str(), 500);
    }
    if (pipe == INVALID_HANDLE_VALUE) {
        return false;
    }

    const QByteArray request = QByteArray(kMagic) + '\t' + token.toLatin1() + '\t'
        + QByteArray::number(parentProcessId) + '\t'
        + prompt.toUtf8().toBase64() + '\n';
    QByteArray reply;
    bool ok = writeAll(pipe, request);
    while (ok && !reply.contains('\n') && reply.size() < kMaxLine) {
        char buffer[4096];
        DWORD read = 0;
        if (!ReadFile(pipe, buffer, sizeof(buffer), &read, nullptr) || read == 0) {
            ok = false;
            break;
        }
        reply.append(buffer, qsizetype(read));
    }
    CloseHandle(pipe);
    if (!ok || !reply.startsWith("OK\t")) {
        return false;
    }
    const QByteArray encoded = reply.mid(3, reply.indexOf('\n') - 3);
    if (answer) {
        *answer = QString::fromUtf8(QByteArray::fromBase64(encoded));
    }
    return true;
}

int runAskpassClient()
{
    int count = 0;
    LPWSTR *arguments = CommandLineToArgvW(GetCommandLineW(), &count);
    if (!arguments) {
        return 1;
    }
    const QString prompt = count >= 2 ? QString::fromWCharArray(arguments[1]) : QString();
    LocalFree(arguments);
    QString answer;
    if (!exchangeAskpassPrompt(qEnvironmentVariable(kAskpassPipeEnv),
                               qEnvironmentVariable(kAskpassTokenEnv),
                               parentProcessId(), prompt, &answer)) {
        return 1;
    }
    HANDLE output = GetStdHandle(STD_OUTPUT_HANDLE);
    if (output == nullptr || output == INVALID_HANDLE_VALUE) {
        return 1;
    }
    return writeAll(output, answer.toUtf8() + '\n') ? 0 : 1;
}

SshAskpassBroker::SshAskpassBroker(QObject *parent)
    : QObject(parent), m_server(new QLocalServer(this)),
      m_program(QDir::toNativeSeparators(QCoreApplication::applicationFilePath()))
{
    // Owner-only DACL: other accounts on the machine cannot open the pipe.
    m_server->setSocketOptions(QLocalServer::UserAccessOption);
    connect(m_server, &QLocalServer::newConnection, this,
            &SshAskpassBroker::acceptConnection);
}

SshAskpassBroker::~SshAskpassBroker()
{
    m_server->close();
}

bool SshAskpassBroker::listen(QString *error)
{
    if (m_server->isListening()) {
        return true;
    }
    const QString name = QStringLiteral("SpanTui-askpass-%1-%2")
                             .arg(QCoreApplication::applicationPid())
                             .arg(randomHex(8));
    if (!m_server->listen(name)) {
        m_listenError = m_server->errorString();
        if (error) {
            *error = m_listenError;
        }
        return false;
    }
    m_listenError.clear();
    return true;
}

QString SshAskpassBroker::listenError() const
{
    return m_listenError;
}

QString SshAskpassBroker::pipeName() const
{
    return m_server->fullServerName();
}

void SshAskpassBroker::setAskpassProgram(const QString &program)
{
    m_program = program;
}

QString SshAskpassBroker::issueToken(const QString &hostId)
{
    const QString token = randomHex(16);
    m_hostByToken.insert(token, hostId);
    return token;
}

void SshAskpassBroker::revokeToken(const QString &token)
{
    m_hostByToken.remove(token);
    const QString prefix = token + QLatin1Char('/');
    for (auto it = m_attempts.begin(); it != m_attempts.end();) {
        it = it.key().startsWith(prefix) ? m_attempts.erase(it) : std::next(it);
    }
}

QHash<QString, QString> SshAskpassBroker::environment(const QString &token) const
{
    return {
        {QStringLiteral("SSH_ASKPASS"), m_program},
        {QStringLiteral("SSH_ASKPASS_REQUIRE"), QStringLiteral("force")},
        {QString::fromLatin1(kAskpassPipeEnv), pipeName()},
        {QString::fromLatin1(kAskpassTokenEnv), token},
    };
}

void SshAskpassBroker::respond(quint64 requestId, bool accepted, const QString &answer)
{
    const Pending pending = m_pending.take(requestId);
    QLocalSocket *socket = pending.socket.data();
    if (!socket) {
        return;
    }
    socket->setProperty(kRequestIdProperty, QVariant());
    socket->write(accepted ? QByteArray("OK\t") + answer.toUtf8().toBase64() + '\n'
                           : QByteArray("NO\n"));
    socket->flush();
    socket->disconnectFromServer();
}

void SshAskpassBroker::acceptConnection()
{
    while (QLocalSocket *socket = m_server->nextPendingConnection()) {
        connect(socket, &QLocalSocket::readyRead, this,
                [this, socket]() { readRequest(socket); });
        connect(socket, &QLocalSocket::disconnected, this, [this, socket]() {
            const QVariant id = socket->property(kRequestIdProperty);
            if (id.isValid() && m_pending.remove(id.toULongLong()) > 0) {
                emit promptAbandoned(id.toULongLong());
            }
            socket->deleteLater();
        });
    }
}

void SshAskpassBroker::readRequest(QLocalSocket *socket)
{
    if (socket->property(kRequestIdProperty).isValid()) {
        return;
    }
    if (!socket->canReadLine()) {
        if (socket->bytesAvailable() > kMaxLine) {
            refuse(socket);
        }
        return;
    }
    const QByteArray line = socket->readLine(kMaxLine).trimmed();
    const QList<QByteArray> fields = line.split('\t');
    if (fields.size() != 4 || fields.at(0) != kMagic) {
        refuse(socket);
        return;
    }
    const QString token = QString::fromLatin1(fields.at(1));
    const auto host = m_hostByToken.constFind(token);
    if (host == m_hostByToken.cend()) {
        refuse(socket);
        return;
    }
    const QString prompt = QString::fromUtf8(QByteArray::fromBase64(fields.at(3)));
    const SshPromptKind kind = classifySshPrompt(prompt);
    const QString attemptKey = token + QLatin1Char('/') + QString::fromLatin1(fields.at(2))
        + QLatin1Char('/') + QString::number(int(kind));
    const int attempt = ++m_attempts[attemptKey];

    const quint64 requestId = m_nextRequestId++;
    socket->setProperty(kRequestIdProperty, requestId);
    m_pending.insert(requestId, Pending{socket});
    const bool launched = isLaunchedSsh(fields.at(2).toUInt(),
                                        quint32(QCoreApplication::applicationPid()));
    emit promptRequested(requestId, host.value(), prompt, int(kind), attempt, launched);
}

void SshAskpassBroker::refuse(QLocalSocket *socket)
{
    socket->write("NO\n");
    socket->flush();
    socket->disconnectFromServer();
}

} // namespace span::remote
