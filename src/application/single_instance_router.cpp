#include "application/single_instance_router.h"

#include "core/product_identity.h"
#include "core/product_paths.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QLocalServer>
#include <QLocalSocket>
#include <QMetaObject>
#include <QStringList>
#include <QThread>
#include <QTimer>
#include <QUuid>

#include <memory>
#include <mutex>
#include <utility>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

namespace span::application {
namespace {

constexpr int kProtocolVersion = 1;
constexpr int kInitialProbeMs = 100;
constexpr int kStartupRaceRetryMs = 3000;
constexpr int kAcknowledgementTimeoutMs = 1500;
// Bound on the main thread's wait for the accept thread to start listening.
constexpr int kListenStartTimeoutMs = 2000;
constexpr int kListenStartPollMs = 5;
constexpr int kConnectAttemptMs = 100;
constexpr int kConnectRetryPauseMs = 25;
// Same-user local sockets only; still bound the request to limit memory DoS.
constexpr qsizetype kMaximumRequestBytes = 64 * 1024;
// Request ids the primary remembers to recognize a retried activation.
constexpr qsizetype kRememberedRequestIds = 32;

QByteArray requestPayload(const QString &projectPath, const QString &requestId)
{
    return QJsonDocument(QJsonObject{
                             {QStringLiteral("version"), kProtocolVersion},
                             {QStringLiteral("projectPath"), projectPath},
                             {QStringLiteral("requestId"), requestId},
                         })
        .toJson(QJsonDocument::Compact)
        + '\n';
}

QByteArray responsePayload(bool accepted, const QString &error = {})
{
    return QJsonDocument(QJsonObject{
                             {QStringLiteral("version"), kProtocolVersion},
                             {QStringLiteral("accepted"), accepted},
                             {QStringLiteral("error"), error},
                         })
        .toJson(QJsonDocument::Compact)
        + '\n';
}

struct ListenResult final {
    std::atomic_bool finished{false};
    std::atomic_bool listening{false};
    std::mutex errorMutex;
    QString error;
};

} // namespace

class SingleInstanceAcceptWorker final : public QObject {
    Q_OBJECT

public:
    SingleInstanceAcceptWorker(QString endpoint, std::shared_ptr<ListenResult> result)
        : m_endpoint(std::move(endpoint)),
          m_result(std::move(result)),
          m_server(new QLocalServer(this))
    {
        m_server->setSocketOptions(QLocalServer::UserAccessOption);
        connect(m_server, &QLocalServer::newConnection, this,
                &SingleInstanceAcceptWorker::acceptConnections);
    }

    void startListening()
    {
        if (m_server->listen(m_endpoint)) {
            m_ownsEndpoint = true;
            m_result->listening.store(true, std::memory_order_release);
        } else {
            {
                const std::lock_guard lock(m_result->errorMutex);
                m_result->error = m_server->errorString();
            }
            m_result->listening.store(false, std::memory_order_release);
        }
        m_result->finished.store(true, std::memory_order_release);
    }

    void stopListening()
    {
        if (m_server->isListening()) {
            m_server->close();
        }
        if (m_ownsEndpoint) {
            QLocalServer::removeServer(m_endpoint);
            m_ownsEndpoint = false;
        }
        thread()->quit();
    }

signals:
    void activationRequested(const QString &projectPath);

private:
    void acceptConnections()
    {
        while (QLocalSocket *socket = m_server->nextPendingConnection()) {
            const auto consumeRequest = [this, socket]() {
                if (socket->property("requestProcessed").toBool()) {
                    return;
                }
                QByteArray payload = socket->property("requestBuffer").toByteArray();
                payload += socket->readAll();
                if (payload.size() > kMaximumRequestBytes) {
                    socket->setProperty("requestProcessed", true);
                    socket->write(responsePayload(
                        false, QStringLiteral("Activation request exceeds size limit.")));
                    socket->flush();
                    socket->disconnectFromServer();
                    return;
                }
                const qsizetype terminator = payload.indexOf('\n');
                if (terminator < 0) {
                    socket->setProperty("requestBuffer", payload);
                    return;
                }
                payload.truncate(terminator);
                socket->setProperty("requestProcessed", true);
                const auto respond = [socket](bool accepted, const QString &error = {}) {
                    socket->write(responsePayload(accepted, error));
                    socket->flush();
                    socket->disconnectFromServer();
                };
                QJsonParseError parseError;
                const QJsonDocument document = QJsonDocument::fromJson(payload, &parseError);
                const QJsonObject object = document.object();
                if (parseError.error != QJsonParseError::NoError
                    || object.value(QStringLiteral("version")).toInt()
                           != kProtocolVersion) {
                    respond(false, QStringLiteral("Unsupported activation request."));
                    return;
                }
                // A secondary that saw no acknowledgement resends the same
                // request; the first copy may already have been served.
                const QString requestId =
                    object.value(QStringLiteral("requestId")).toString();
                if (!requestId.isEmpty() && m_recentRequestIds.contains(requestId)) {
                    respond(true);
                    return;
                }
                const QString requested =
                    object.value(QStringLiteral("projectPath")).toString();
                if (!requested.isEmpty()) {
                    const QFileInfo info(requested);
                    if (!info.isAbsolute() || !info.exists() || !info.isDir()) {
                        respond(false, QStringLiteral("Requested project is not a directory."));
                        return;
                    }
                    emit activationRequested(QDir::cleanPath(info.canonicalFilePath()));
                } else {
                    emit activationRequested(QString());
                }
                if (!requestId.isEmpty()) {
                    m_recentRequestIds.append(requestId);
                    if (m_recentRequestIds.size() > kRememberedRequestIds)
                        m_recentRequestIds.removeFirst();
                }
                respond(true);
            };
            connect(socket, &QLocalSocket::readyRead, socket, consumeRequest);
            connect(socket, &QLocalSocket::disconnected, socket,
                    [socket, consumeRequest]() {
                        consumeRequest();
                        socket->deleteLater();
                    });
            QTimer::singleShot(0, socket, consumeRequest);
        }
    }

    QString m_endpoint;
    std::shared_ptr<ListenResult> m_result;
    QLocalServer *m_server = nullptr;
    bool m_ownsEndpoint = false;
    QStringList m_recentRequestIds;
};

SingleInstanceRouter::SingleInstanceRouter(QObject *parent, QString endpointOverride)
    : QObject(parent)
{
    if (!endpointOverride.isEmpty()) {
        m_endpointName = std::move(endpointOverride);
    } else {
        // Data root plus image name: a dev SpanTui-test.exe must not forward
        // into the installed SpanTui.exe.
        const QByteArray identity =
            (span::core::ProductPaths::root().toLower()
             + QLatin1Char('|')
             + span::core::productImageName().toLower()
             + QStringLiteral("|v%1").arg(kProtocolVersion))
                .toUtf8();
        const QByteArray digest =
            QCryptographicHash::hash(identity, QCryptographicHash::Sha256).toHex().left(32);
        m_endpointName = QStringLiteral("SpanTui-app-%1").arg(QString::fromLatin1(digest));
    }
}

SingleInstanceRouter::~SingleInstanceRouter()
{
    stopPrimary();
}

QString SingleInstanceRouter::endpointName() const
{
    return m_endpointName;
}

void SingleInstanceRouter::stopPrimary()
{
    m_primaryListening.store(false, std::memory_order_release);
    if (m_primaryLock) {
        CloseHandle(std::exchange(m_primaryLock, nullptr));
    }
    if (!m_acceptThread) {
        return;
    }
    SingleInstanceAcceptWorker *worker = std::exchange(m_acceptWorker, nullptr);
    QThread *acceptThread = std::exchange(m_acceptThread, nullptr);
    disconnect(worker, nullptr, this, nullptr);
    acceptThread->setParent(nullptr);
    connect(acceptThread, &QThread::finished, worker, &QObject::deleteLater);
    connect(acceptThread, &QThread::finished, acceptThread, &QObject::deleteLater);
    if (!QMetaObject::invokeMethod(worker, &SingleInstanceAcceptWorker::stopListening,
                                   Qt::QueuedConnection)) {
        // A thread which has already stopped cannot service the cleanup command.
        // Do not destroy its thread-affine objects from the GUI thread.
        acceptThread->quit();
    }
}

bool SingleInstanceRouter::becomePrimary(QString *errorMessage)
{
    if (m_acceptThread) {
        return m_primaryListening.load(std::memory_order_acquire);
    }

    // QLocalServer creates its pipe instances without FILE_FLAG_FIRST_PIPE_INSTANCE,
    // so listen() also succeeds while another process serves the endpoint. A
    // named mutex makes the primary role exclusive; the kernel drops it with
    // the owning process, so a crashed primary never blocks the next start.
    const std::wstring lockName =
        (QStringLiteral("Local\\") + m_endpointName).toStdWString();
    HANDLE lock = CreateMutexW(nullptr, FALSE, lockName.c_str());
    if (!lock || GetLastError() == ERROR_ALREADY_EXISTS) {
        if (lock) {
            CloseHandle(lock);
        }
        if (errorMessage) {
            *errorMessage = QStringLiteral("Another instance owns the single-instance endpoint.");
        }
        return false;
    }
    m_primaryLock = lock;

    // Own the server on a dedicated thread so activations are accepted during
    // the listen-before-exec cold-start window on the main thread.
    const auto result = std::make_shared<ListenResult>();
    m_acceptThread = new QThread(this);
    m_acceptWorker = new SingleInstanceAcceptWorker(m_endpointName, result);
    m_acceptWorker->moveToThread(m_acceptThread);
    connect(m_acceptThread, &QThread::started, m_acceptWorker,
            &SingleInstanceAcceptWorker::startListening);
    connect(m_acceptWorker, &SingleInstanceAcceptWorker::activationRequested,
            this, &SingleInstanceRouter::activationRequested);

    m_acceptThread->start();

    QElapsedTimer wait;
    wait.start();
    while (!result->finished.load(std::memory_order_acquire) && wait.elapsed() < kListenStartTimeoutMs) {
        QThread::msleep(kListenStartPollMs);
    }

    if (result->listening.load(std::memory_order_acquire)) {
        m_primaryListening.store(true, std::memory_order_release);
        return true;
    }

    QString listenError;
    {
        const std::lock_guard lock(result->errorMutex);
        listenError = result->error;
    }
    stopPrimary();
    if (errorMessage) {
        *errorMessage = listenError.isEmpty()
                            ? QStringLiteral("Failed to listen for single-instance activations.")
                            : listenError;
    }
    return false;
}

SingleInstanceRouter::ForwardOutcome SingleInstanceRouter::forward(
    const QString &projectPath, const QString &requestId, int retryWindowMs,
    QString *errorMessage)
{
    QLocalSocket socket;
    QElapsedTimer retryWindow;
    retryWindow.start();
    do {
        socket.abort();
        socket.connectToServer(m_endpointName, QIODevice::ReadWrite);
        const int remaining = qMax(1, retryWindowMs - int(retryWindow.elapsed()));
        if (socket.waitForConnected(qMin(kConnectAttemptMs, remaining))) {
            break;
        }
        if (retryWindow.elapsed() < retryWindowMs) {
            QThread::msleep(kConnectRetryPauseMs);
        }
    } while (retryWindow.elapsed() < retryWindowMs);
    if (socket.state() != QLocalSocket::ConnectedState) {
        if (errorMessage) {
            *errorMessage = socket.errorString();
        }
        return ForwardOutcome::Unreachable;
    }
    // Only a refused write counts as unreachable. Not waitForBytesWritten():
    // on Windows it returns false for a write that completed synchronously.
    // The acknowledgement wait below drives the write.
    const QByteArray payload = requestPayload(projectPath, requestId);
    if (socket.write(payload) != payload.size()) {
        if (errorMessage) {
            *errorMessage = socket.errorString();
        }
        return ForwardOutcome::Unreachable;
    }

    QByteArray response;
    QElapsedTimer acknowledgementWindow;
    acknowledgementWindow.start();
    while (!response.contains('\n')
           && acknowledgementWindow.elapsed() < kAcknowledgementTimeoutMs) {
        response += socket.readAll();
        if (response.contains('\n')) {
            break;
        }
        if (socket.state() != QLocalSocket::ConnectedState
            && socket.bytesAvailable() == 0) {
            // The primary died or hung up. Never spin on a dead socket until
            // the full timeout: that burned a core in the GUI thread.
            break;
        }
        const int remaining =
            kAcknowledgementTimeoutMs - int(acknowledgementWindow.elapsed());
        if (remaining <= 0
            || !socket.waitForReadyRead(qMax(1, remaining))) {
            if (socket.state() != QLocalSocket::ConnectedState
                && socket.bytesAvailable() == 0) {
                break;
            }
        }
    }
    const qsizetype terminator = response.indexOf('\n');
    if (terminator < 0) {
        // Connected and wrote, but no acknowledgement yet. Keep this as a
        // provisional forward so a slow primary does not surface as failure.
        if (errorMessage) {
            errorMessage->clear();
        }
        return ForwardOutcome::Unacknowledged;
    }
    response.truncate(terminator);
    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(response, &parseError);
    const QJsonObject object = document.object();
    if (parseError.error != QJsonParseError::NoError
        || object.value(QStringLiteral("version")).toInt() != kProtocolVersion) {
        if (errorMessage) {
            *errorMessage = QStringLiteral(
                "Primary instance returned an unsupported activation response.");
        }
        return ForwardOutcome::Rejected;
    }
    if (!object.value(QStringLiteral("accepted")).toBool()) {
        if (errorMessage) {
            *errorMessage = object.value(QStringLiteral("error")).toString(
                QStringLiteral("Primary instance rejected the activation request."));
        }
        return ForwardOutcome::Rejected;
    }
    if (errorMessage) {
        errorMessage->clear();
    }
    return ForwardOutcome::Accepted;
}

SingleInstanceRouter::StartResult SingleInstanceRouter::startOrForward(
    const QString &projectPath, QString *errorMessage)
{
    // Every send of this activation carries one id, so a primary that
    // served an unacknowledged attempt ignores the retry.
    const QString requestId = QUuid::createUuid().toString(QUuid::WithoutBraces);
    const ForwardOutcome first =
        forward(projectPath, requestId, kInitialProbeMs, errorMessage);
    if (first == ForwardOutcome::Accepted) {
        return StartResult::Forwarded;
    }
    if (first == ForwardOutcome::Rejected) {
        return StartResult::Rejected;
    }
    // The primary may be mid-startup. An unanswered write is not success: it
    // would drop the activation.
    if (first == ForwardOutcome::Unacknowledged) {
        const ForwardOutcome ackRetry =
            forward(projectPath, requestId, kStartupRaceRetryMs, errorMessage);
        if (ackRetry == ForwardOutcome::Accepted) {
            return StartResult::Forwarded;
        }
        if (ackRetry == ForwardOutcome::Rejected) {
            return StartResult::Rejected;
        }
        if (errorMessage) {
            *errorMessage = QStringLiteral(
                "Primary instance did not acknowledge the activation request.");
        }
        return StartResult::Failed;
    }

    if (becomePrimary(errorMessage)) {
        return StartResult::Primary;
    }

    const ForwardOutcome retry =
        forward(projectPath, requestId, kStartupRaceRetryMs, errorMessage);
    if (retry == ForwardOutcome::Accepted) {
        return StartResult::Forwarded;
    }
    if (retry == ForwardOutcome::Rejected) {
        return StartResult::Rejected;
    }
    if (retry == ForwardOutcome::Unacknowledged) {
        if (errorMessage) {
            *errorMessage = QStringLiteral(
                "Primary instance did not acknowledge the activation request.");
        }
        return StartResult::Failed;
    }
    if (errorMessage && errorMessage->isEmpty()) {
        *errorMessage = QStringLiteral("Cannot acquire single-instance endpoint.");
    }
    return StartResult::Failed;
}

} // namespace span::application

#include "single_instance_router.moc"
