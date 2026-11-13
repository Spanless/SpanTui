#include "application/single_instance_router.h"

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QLocalServer>
#include <QLocalSocket>
#include <QProcess>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QThread>
#include <QUuid>
#include <QtTest>

using span::application::SingleInstanceRouter;

class SingleInstanceRouterTest final : public QObject {
    Q_OBJECT

private slots:
    void forwardsProjectToPrimaryInstance();
    void coldPrimaryStillAcceptsForward();
    void rejectsMissingDirectoryWithoutBecomingPrimary();
    void secondaryDoesNotBecomePrimaryWhileEndpointHeld();
    void failsWhenPrimaryNeverAcknowledges();
    void rejectsOversizedActivationRequest();
    void retriedRequestActivatesOnce();
    void defaultEndpointIncludesProductImageName();
    void destructionDoesNotWaitForAcceptThread();
};

namespace {

QString uniqueEndpoint()
{
    return QStringLiteral("SpanTui-router-test-%1")
        .arg(QUuid::createUuid().toString(QUuid::WithoutBraces));
}

// Reads and discards every request on `server`'s pending connections without
// ever answering, as a primary that hangs would.
void swallowPendingConnections(QLocalServer *server)
{
    while (QLocalSocket *socket = server->nextPendingConnection()) {
        QObject::connect(socket, &QLocalSocket::readyRead, socket, [socket]() { Q_UNUSED(socket->readAll()); });
    }
}

// Sends `request` on a new connection to `endpoint`; returns the first
// response line, or empty when there is none within the timeouts.
// No waitForBytesWritten(): the primary can read, answer and hang up before
// it returns, and it then reports false with the answer already buffered.
// The response wait drives the write, as in the router's forward path.
QByteArray exchange(const QString &endpoint, const QByteArray &request)
{
    QLocalSocket client;
    client.connectToServer(endpoint);
    if (!client.waitForConnected(2000) || client.write(request) != request.size()) return {};
    QByteArray response;
    // A partial line that never completes counts as no response.
    return QTest::qWaitFor([&]() { return (response += client.readAll()).contains('\n'); }, 3000) ? response
                                                                                                  : QByteArray();
}

QString helperExecutable()
{
    return QDir(QCoreApplication::applicationDirPath())
        .filePath(QStringLiteral("single_instance_client_helper.exe"));
}

int runHelper(const QString &endpoint, const QString &projectPath,
              QString *stderrText = nullptr)
{
    const QString helper = helperExecutable();
    if (!QFileInfo::exists(helper)) {
        return -1;
    }
    QProcess secondary;
    QSignalSpy finished(
        &secondary,
        qOverload<int, QProcess::ExitStatus>(&QProcess::finished));
    secondary.start(helper, {endpoint, projectPath});
    if (!secondary.waitForStarted(5000)) {
        if (stderrText) {
            *stderrText = secondary.errorString();
        }
        return -1;
    }
    // QSignalSpy::wait pumps the primary event loop while the helper is blocked
    // on an acknowledgement.
    if (!finished.wait(8000)) {
        if (stderrText) {
            *stderrText = secondary.errorString();
        }
        secondary.kill();
        secondary.waitForFinished(1000);
        return -1;
    }
    if (stderrText) {
        *stderrText = QString::fromLocal8Bit(secondary.readAllStandardError());
    }
    if (secondary.exitStatus() != QProcess::NormalExit) {
        return -1;
    }
    return secondary.exitCode();
}

} // namespace

void SingleInstanceRouterTest::forwardsProjectToPrimaryInstance()
{
    QTemporaryDir project;
    QVERIFY(project.isValid());
    const QString endpoint = uniqueEndpoint();

    SingleInstanceRouter primary(nullptr, endpoint);
    QString error;
    QElapsedTimer acquisition;
    acquisition.start();
    QCOMPARE(primary.startOrForward(QString(), &error),
             SingleInstanceRouter::StartResult::Primary);
    QVERIFY2(acquisition.elapsed() < 1000,
             qPrintable(QStringLiteral("Primary acquisition took %1 ms")
                            .arg(acquisition.elapsed())));
    QSignalSpy activation(&primary, &SingleInstanceRouter::activationRequested);

    QString stderrText;
    QCOMPARE(runHelper(endpoint, project.path(), &stderrText), 0);
    QTRY_COMPARE_WITH_TIMEOUT(activation.count(), 1, 5000);
    QCOMPARE(activation.takeFirst().at(0).toString(),
             QDir(project.path()).canonicalPath());
}

void SingleInstanceRouterTest::coldPrimaryStillAcceptsForward()
{
    QTemporaryDir project;
    QVERIFY(project.isValid());
    const QString endpoint = uniqueEndpoint();

    SingleInstanceRouter primary(nullptr, endpoint);
    QString error;
    QCOMPARE(primary.startOrForward(QString(), &error),
             SingleInstanceRouter::StartResult::Primary);
    // Spy before the secondary launches so cross-thread activations are not missed.
    QSignalSpy activation(&primary, &SingleInstanceRouter::activationRequested);

    const QString helper = helperExecutable();
    QVERIFY2(QFileInfo::exists(helper), qPrintable(helper));

    QProcess secondary;
    secondary.start(helper, {endpoint, project.path()});
    QVERIFY2(secondary.waitForStarted(5000), qPrintable(secondary.errorString()));

    // Main thread is not pumping events. The accept thread must still acknowledge
    // so the helper exits 0 and activation is delivered.
    QVERIFY2(secondary.waitForFinished(5000), qPrintable(secondary.errorString()));
    QCOMPARE(secondary.exitStatus(), QProcess::NormalExit);
    QVERIFY2(secondary.exitCode() == 0,
             secondary.readAllStandardError().constData());

    QTRY_COMPARE_WITH_TIMEOUT(activation.count(), 1, 5000);
    QCOMPARE(activation.takeFirst().at(0).toString(),
             QDir(project.path()).canonicalPath());
}

void SingleInstanceRouterTest::rejectsMissingDirectoryWithoutBecomingPrimary()
{
    const QString endpoint = uniqueEndpoint();
    SingleInstanceRouter primary(nullptr, endpoint);
    QString error;
    QCOMPARE(primary.startOrForward(QString(), &error),
             SingleInstanceRouter::StartResult::Primary);
    QSignalSpy activation(&primary, &SingleInstanceRouter::activationRequested);

    const QString missing = QDir::temp().filePath(
        QStringLiteral("span-tui-missing-%1")
            .arg(QUuid::createUuid().toString(QUuid::WithoutBraces)));
    QVERIFY(!QFileInfo::exists(missing));

    QString stderrText;
    int exitCode = runHelper(endpoint, missing, &stderrText);
    QVERIFY2(exitCode == 2, qPrintable(QStringLiteral("exit %1: %2").arg(exitCode).arg(stderrText)));
    QCOMPARE(activation.count(), 0);

    // A later activation with no project path still reaches the same primary
    // (rejected secondaries must not have stolen the endpoint).
    exitCode = runHelper(endpoint, QString(), &stderrText);
    QVERIFY2(exitCode == 0, qPrintable(QStringLiteral("exit %1: %2").arg(exitCode).arg(stderrText)));
    QTRY_COMPARE_WITH_TIMEOUT(activation.count(), 1, 5000);
    QCOMPARE(activation.takeFirst().at(0).toString(), QString());
}

void SingleInstanceRouterTest::secondaryDoesNotBecomePrimaryWhileEndpointHeld()
{
    QTemporaryDir project;
    QVERIFY(project.isValid());
    const QString endpoint = uniqueEndpoint();
    SingleInstanceRouter primary(nullptr, endpoint);
    QString error;
    QCOMPARE(primary.startOrForward(QString(), &error),
             SingleInstanceRouter::StartResult::Primary);
    QSignalSpy activation(&primary, &SingleInstanceRouter::activationRequested);

    QString stderrText;
    QCOMPARE(runHelper(endpoint, project.path(), &stderrText), 0);
    QCOMPARE(runHelper(endpoint, QString(), &stderrText), 0);
    QTRY_COMPARE_WITH_TIMEOUT(activation.count(), 2, 5000);
    QCOMPARE(activation.takeFirst().at(0).toString(),
             QDir(project.path()).canonicalPath());
    QCOMPARE(activation.takeFirst().at(0).toString(), QString());
}

void SingleInstanceRouterTest::failsWhenPrimaryNeverAcknowledges()
{
    const QString endpoint = uniqueEndpoint();
    // Accept on a dedicated thread so the secondary can block in startOrForward
    // without starving the silent primary's event loop.
    QThread serverThread;
    auto *silent = new QLocalServer;
    silent->setSocketOptions(QLocalServer::UserAccessOption);
    silent->moveToThread(&serverThread);
    QObject::connect(&serverThread, &QThread::started, silent, [silent, endpoint]() {
        silent->listen(endpoint);
    });
    QObject::connect(silent, &QLocalServer::newConnection, silent, [silent]() { swallowPendingConnections(silent); });
    serverThread.start();
    QTRY_VERIFY_WITH_TIMEOUT(silent->isListening(), 2000);

    SingleInstanceRouter secondary(nullptr, endpoint);
    QString error;
    QCOMPARE(secondary.startOrForward(QString(), &error),
             SingleInstanceRouter::StartResult::Failed);
    QVERIFY2(error.contains(QStringLiteral("acknowledge")), qPrintable(error));

    QMetaObject::invokeMethod(
        silent,
        [silent]() {
            silent->close();
            QLocalServer::removeServer(silent->serverName());
            silent->deleteLater();
        },
        Qt::BlockingQueuedConnection);
    serverThread.quit();
    QVERIFY(serverThread.wait(3000));
}

void SingleInstanceRouterTest::rejectsOversizedActivationRequest()
{
    const QString endpoint = uniqueEndpoint();
    SingleInstanceRouter primary(nullptr, endpoint);
    QString error;
    QCOMPARE(primary.startOrForward(QString(), &error),
             SingleInstanceRouter::StartResult::Primary);
    QSignalSpy activation(&primary, &SingleInstanceRouter::activationRequested);

    QLocalSocket client;
    client.connectToServer(endpoint);
    QVERIFY(client.waitForConnected(2000));
    // Larger than the 64KiB request ceiling and intentionally without '\n'.
    const QByteArray huge(70 * 1024, 'A');
    QCOMPARE(client.write(huge), huge.size());
    QTRY_VERIFY_WITH_TIMEOUT(
        client.state() == QLocalSocket::UnconnectedState
            || client.bytesAvailable() > 0,
        3000);
    QCOMPARE(activation.count(), 0);
}

void SingleInstanceRouterTest::retriedRequestActivatesOnce()
{
    QTemporaryDir project;
    QVERIFY(project.isValid());
    const QString endpoint = uniqueEndpoint();
    SingleInstanceRouter primary(nullptr, endpoint);
    QString error;
    QCOMPARE(primary.startOrForward(QString(), &error),
             SingleInstanceRouter::StartResult::Primary);
    QSignalSpy activation(&primary, &SingleInstanceRouter::activationRequested);

    // A secondary that got no acknowledgement resends the same request.
    const QByteArray request =
        QStringLiteral(R"({"version":1,"projectPath":"%1","requestId":"retry-1"})")
            .arg(QDir::fromNativeSeparators(project.path()))
            .toUtf8()
        + '\n';
    const QByteArray first = exchange(endpoint, request);
    QVERIFY2(first.contains(R"("accepted":true)"), first.constData());
    const QByteArray retried = exchange(endpoint, request);
    QVERIFY2(retried.contains(R"("accepted":true)"), retried.constData());
    QTRY_COMPARE_WITH_TIMEOUT(activation.count(), 1, 3000);
    QTest::qWait(100);
    QCOMPARE(activation.count(), 1);
}

void SingleInstanceRouterTest::defaultEndpointIncludesProductImageName()
{
    // Same process image + AppData → same endpoint. Distinct PE basenames
    // (SpanTui-test vs SpanTui) produce different digests so dev never
    // forwards into the installed desktop instance.
    SingleInstanceRouter first;
    SingleInstanceRouter second;
    QCOMPARE(first.endpointName(), second.endpointName());
    QVERIFY(first.endpointName().startsWith(QStringLiteral("SpanTui-app-")));
}

void SingleInstanceRouterTest::destructionDoesNotWaitForAcceptThread()
{
    auto *router = new SingleInstanceRouter(nullptr, uniqueEndpoint());
    QString error;
    QCOMPARE(router->startOrForward(QString(), &error),
             SingleInstanceRouter::StartResult::Primary);
    QElapsedTimer destruction;
    destruction.start();
    delete router;
    QVERIFY2(destruction.elapsed() < 100,
             "SingleInstanceRouter destruction waited for accept thread");
}

QTEST_MAIN(SingleInstanceRouterTest)

#include "single_instance_router_test.moc"
