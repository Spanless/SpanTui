#include "application/gui_application.h"
#include "application/message_center.h"

#include <QEvent>
#include <QMessageLogContext>
#include <QRegularExpression>
#include <QScopeGuard>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QSignalSpy>
#include <QTest>
#include <QThread>
#include <QTemporaryDir>

#include <stdexcept>

using span::application::GuiApplication;
using span::application::MessageCenter;
using span::application::MessageKind;

namespace {

void postNumbered(MessageCenter *center, int count)
{
    for (int i = 0; i < count; ++i) center->post(MessageKind::Info, QStringLiteral("row %1").arg(i));
}

class ThrowingObject final : public QObject {
protected:
    void customEvent(QEvent *) override
    {
        throw std::runtime_error("boom from event");
    }
};

class WorkerObject final : public QObject {
    Q_OBJECT

public:
    void postFromThisThread(const QString &body)
    {
        if (auto *center = MessageCenter::instance())
            center->post(MessageKind::Error, body);
    }
};

} // namespace

class MessageCenterTest final : public QObject {
    Q_OBJECT

private slots:
    void init();
    void postsAndTracksUnread();
    void keepsEveryDiagnosticEntry();
    void capsStoredEntries();
    void autoHidesEveryToast();
    void openingInboxMarksReadAndHidesToasts();
    void postsFromWorkerThread();
    void notifySwallowsCppExceptions();
    void logSinkForwardsEveryWarningAndCritical();
    void warningDuringInsertIsDeferredNotNested();
    void emptyOrWhitespaceBodyIsIgnored();
    void unknownKindFallsBackToInfo();
    void persistsMessagesAndReadState();

private:
    MessageCenter *center() const
    {
        return MessageCenter::instance();
    }
};

void MessageCenterTest::init()
{
    QVERIFY(center());
    // Drain queued diagnostics from the previous case before resetting.
    QCoreApplication::processEvents();
    center()->clear();
    center()->setInboxOpen(false);
}

void MessageCenterTest::postsAndTracksUnread()
{
    QSignalSpy unread(center(), &MessageCenter::unreadCountChanged);
    center()->post(MessageKind::Error, QStringLiteral("disk failed"));
    center()->post(MessageKind::Success, QStringLiteral("saved"));
    QCOMPARE(center()->count(), 2);
    QCOMPARE(center()->unreadCount(), 2);
    QCOMPARE(center()->unreadErrorCount(), 1);
    QVERIFY(unread.count() >= 1);
    QCOMPARE(center()->data(center()->index(0), MessageCenter::KindRole)
                 .toString(),
             QStringLiteral("success"));
    QCOMPARE(center()->data(center()->index(1), MessageCenter::BodyRole)
                 .toString(),
             QStringLiteral("disk failed"));
}

void MessageCenterTest::keepsEveryDiagnosticEntry()
{
    center()->post(MessageKind::Error, QStringLiteral("same"));
    center()->post(MessageKind::Error, QStringLiteral("same"));
    QCOMPARE(center()->count(), 2);

    center()->post(MessageKind::Warning, QStringLiteral("same"));
    center()->post(MessageKind::Warning, QStringLiteral("same"));
    QCOMPARE(center()->count(), 4);

    center()->post(MessageKind::Info, QStringLiteral("notice"));
    center()->post(MessageKind::Info, QStringLiteral("notice"));
    QCOMPARE(center()->count(), 5);

    center()->post(MessageKind::Success, QStringLiteral("saved"));
    center()->post(MessageKind::Success, QStringLiteral("saved"));
    QCOMPARE(center()->count(), 6);
}

void MessageCenterTest::capsStoredEntries()
{
    postNumbered(center(), 210);
    QCOMPARE(center()->count(), 200);
    QCOMPARE(center()->data(center()->index(0), MessageCenter::BodyRole)
                 .toString(),
             QStringLiteral("row 209"));
}

void MessageCenterTest::autoHidesEveryToast()
{
    // Five seconds in the product; the timer, not its length, is under test.
    center()->setAutoDismissMsForTesting(100);
    const auto restore = qScopeGuard([this]() { center()->setAutoDismissMsForTesting(5000); });
    center()->post(MessageKind::Success, QStringLiteral("ok"));
    center()->post(MessageKind::Error, QStringLiteral("bad"));
    QCOMPARE(center()->toasts().size(), 2);
    QTRY_COMPARE_WITH_TIMEOUT(center()->toasts().size(), 0, 1000);
    QCOMPARE(center()->count(), 2);
}

void MessageCenterTest::openingInboxMarksReadAndHidesToasts()
{
    center()->post(MessageKind::Error, QStringLiteral("keep in inbox"));
    QCOMPARE(center()->unreadCount(), 1);
    QVERIFY(!center()->toasts().isEmpty());
    center()->setInboxOpen(true);
    QCOMPARE(center()->unreadCount(), 0);
    QVERIFY(center()->toasts().isEmpty());
    QCOMPARE(center()->count(), 1);
    center()->setInboxOpen(false);
}

void MessageCenterTest::postsFromWorkerThread()
{
    WorkerObject worker;
    QThread thread;
    worker.moveToThread(&thread);
    thread.start();
    QVERIFY(QMetaObject::invokeMethod(
        &worker, [&worker]() {
            worker.postFromThisThread(QStringLiteral("from worker"));
        },
        Qt::BlockingQueuedConnection));
    QTRY_COMPARE(center()->count(), 1);
    QCOMPARE(center()->data(center()->index(0), MessageCenter::BodyRole)
                 .toString(),
             QStringLiteral("from worker"));
    thread.quit();
    QVERIFY(thread.wait(2000));
}

void MessageCenterTest::logSinkForwardsEveryWarningAndCritical()
{
    const QMessageLogContext qmlContext(
        __FILE__, __LINE__, Q_FUNC_INFO, "qt.qml.binding");
    center()->postFromLog(
        QtWarningMsg, qmlContext,
        QStringLiteral("qrc:/x.qml:1: Unable to assign [undefined] to double"));
    // Log delivery is always queued so a warning raised by a model consumer
    // can never re-enter the model synchronously.
    QCOMPARE(center()->count(), 0);
    QTRY_COMPARE(center()->count(), 1);

    center()->postFromLog(
        QtWarningMsg, qmlContext,
        QStringLiteral("DelegateModel::cancel: index out range 19 0"));
    QTRY_COMPARE(center()->count(), 2);

    const QMessageLogContext appContext(
        __FILE__, __LINE__, Q_FUNC_INFO, "default");
    center()->postFromLog(
        QtWarningMsg, appContext,
        QStringLiteral(
            "QFileSystemWatcher: FindNextChangeNotification failed for ID: "
            "C:/project/.tmp/imp"));
    QTRY_COMPARE(center()->count(), 3);

    const QMessageLogContext fontContext(
        __FILE__, __LINE__, Q_FUNC_INFO, "qt.text.font.db");
    center()->postFromLog(QtWarningMsg, fontContext,
                          QStringLiteral("OpenType support missing"));
    QTRY_COMPARE(center()->count(), 4);

    center()->postFromLog(QtWarningMsg, appContext,
                          QStringLiteral("Application error: already posted"));
    QTRY_COMPARE(center()->count(), 5);

    center()->postFromLog(
        QtWarningMsg, appContext,
        QStringLiteral("Terminal preferences error: already posted"));
    QTRY_COMPARE(center()->count(), 6);

    center()->postFromLog(QtCriticalMsg, appContext,
                          QStringLiteral("persistence failed"));
    QTRY_COMPARE(center()->count(), 7);
    QCOMPARE(center()->data(center()->index(0), MessageCenter::KindRole)
                 .toString(),
             QStringLiteral("error"));
    QCOMPARE(center()->unreadErrorCount(), 1);
}

void MessageCenterTest::warningDuringInsertIsDeferredNotNested()
{
    center()->attachToQtLogging();
    int inserts = 0;
    const auto connection = connect(
        center(), &QAbstractItemModel::rowsInserted, this,
        [&](const QModelIndex &, int first, int last) {
            ++inserts;
            QCOMPARE(first, 0);
            QCOMPARE(last, 0);
            QCOMPARE(center()->rowCount(), inserts);
        });
    // Connected second, so it runs after the counter for the first insert only.
    connect(
        center(), &QAbstractItemModel::rowsInserted, this, []() { qWarning("warning raised by a model consumer"); },
        Qt::SingleShotConnection);

    center()->post(MessageKind::Error, QStringLiteral("first"));
    QCOMPARE(center()->count(), 1);
    QCOMPARE(inserts, 1);
    QTRY_COMPARE(center()->count(), 2);
    QCOMPARE(inserts, 2);
    QCOMPARE(center()->data(center()->index(0), MessageCenter::BodyRole)
                 .toString(),
             QStringLiteral("warning raised by a model consumer"));
    disconnect(connection);
}

void MessageCenterTest::emptyOrWhitespaceBodyIsIgnored()
{
    center()->post(MessageKind::Error, QString());
    center()->post(MessageKind::Info, QStringLiteral("   "));
    center()->post(QStringLiteral("error"), QStringLiteral("\t\n"));
    QCOMPARE(center()->count(), 0);
    QCOMPARE(center()->unreadCount(), 0);
}

void MessageCenterTest::unknownKindFallsBackToInfo()
{
    center()->post(QStringLiteral("not-a-kind"), QStringLiteral("plain notice"));
    QCOMPARE(center()->count(), 1);
    QCOMPARE(center()->data(center()->index(0), MessageCenter::KindRole)
                 .toString(),
             QStringLiteral("info"));
}

void MessageCenterTest::persistsMessagesAndReadState()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString databasePath =
        temporary.filePath(QStringLiteral("messages.db"));
    const QString setupConnection =
        QStringLiteral("message_center_setup_%1")
            .arg(static_cast<qulonglong>(
                     reinterpret_cast<quintptr>(this)),
                 0, 16);
    {
        QSqlDatabase database = QSqlDatabase::addDatabase(
            QStringLiteral("QSQLITE"), setupConnection);
        database.setDatabaseName(databasePath);
        QVERIFY(database.open());
        QSqlQuery query(database);
        QVERIFY(query.exec(QStringLiteral(
            "create table messages ("
            "id integer primary key autoincrement, "
            "kind text not null, title text not null, body text not null, "
            "created_at integer not null, read integer not null default 0)")));
        database.close();
    }
    QSqlDatabase::removeDatabase(setupConnection);

    {
        MessageCenter writer;
        QVERIFY(writer.initializePersistence(databasePath));
        writer.post(MessageKind::Error, QStringLiteral("disk failed"),
                    QStringLiteral("Write failed"));
        writer.post(MessageKind::Warning, QStringLiteral("watcher failed"));
    }

    {
        MessageCenter reader;
        QVERIFY(reader.initializePersistence(databasePath));
        QCOMPARE(reader.count(), 2);
        QCOMPARE(reader.unreadCount(), 2);
        QVERIFY(reader.toasts().isEmpty());
        QCOMPARE(reader.data(reader.index(0), MessageCenter::BodyRole)
                     .toString(),
                 QStringLiteral("watcher failed"));
        reader.setInboxOpen(true);
    }

    {
        MessageCenter reader;
        QVERIFY(reader.initializePersistence(databasePath));
        QCOMPARE(reader.count(), 2);
        QCOMPARE(reader.unreadCount(), 0);
        reader.clear();
    }

    MessageCenter emptyReader;
    QVERIFY(emptyReader.initializePersistence(databasePath));
    QCOMPARE(emptyReader.count(), 0);

    // A folder is no database: persistence turns off once, with a warning.
    MessageCenter unopenable;
    QTest::ignoreMessage(QtWarningMsg,
                         QRegularExpression(QStringLiteral("Message persistence is unavailable")));
    QVERIFY(!unopenable.initializePersistence(temporary.path()));
    QVERIFY(!unopenable.initializePersistence(temporary.path()));
    unopenable.post(MessageKind::Info, QStringLiteral("kept in memory"));
    QCOMPARE(unopenable.count(), 1);
}

void MessageCenterTest::notifySwallowsCppExceptions()
{
    ThrowingObject thrower;
    QEvent event(QEvent::User);
    QVERIFY(QCoreApplication::sendEvent(&thrower, &event));
    QCOMPARE(center()->count(), 1);
    QCOMPARE(center()->data(center()->index(0), MessageCenter::KindRole)
                 .toString(),
             QStringLiteral("error"));
    QVERIFY(center()->data(center()->index(0), MessageCenter::BodyRole)
                .toString()
                .contains(QStringLiteral("boom from event")));
}

int main(int argc, char **argv)
{
    GuiApplication app(argc, argv);
    MessageCenter center;
    MessageCenterTest test;
    return QTest::qExec(&test, argc, argv);
}

#include "message_center_test.moc"
