#include "application/gui_application.h"
#include "application/message_center.h"
#include "application/settings_store.h"
#include "application/task_progress.h"
#include "application/terminal_host_events.h"
#include "application/workspace_messages.h"
#include "documents/document_manager.h"
#include "git/git_repository_service.h"
#include "remote/remote_file_model.h"
#include "terminal/api/terminal_manager.h"
#include "workspace/file_tree_model.h"
#include "workspace/session_model.h"

#include <QFile>
#include <QFileInfo>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTest>

#include <memory>

using span::application::GuiApplication;
using span::application::MessageCenter;

namespace {

void writeBytes(const QString &path, const QByteArray &bytes)
{
    QFile file(path);
    QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
    QCOMPARE(file.write(bytes), bytes.size());
}

} // namespace

// Drives the production wiring (application/workspace_messages.h) with real
// domain owners and observes the MessageCenter rows it produces.
class MessageCenterWiringTest final : public QObject {
    Q_OBJECT

private slots:
    void init();
    void documentErrorsReachTheCenter();
    void fileTreeOperationsPostSuccess();
    void fileTreeRefusalsPostErrors();
    void gitRefusalsPostErrors();
    void remoteFileRefusalsPostErrors();
    void clearingAnErrorDoesNotPostAgain();
    void connectionEndsWithItsLifetimeOwner();
    void terminalProgressAndNotificationsReachTheHost();

private:
    MessageCenter *center() const { return MessageCenter::instance(); }
    QString kindAt(int row) const
    {
        return center()->data(center()->index(row), MessageCenter::KindRole).toString();
    }
};

void MessageCenterWiringTest::init()
{
    QStandardPaths::setTestModeEnabled(true);
    QVERIFY(center());
    center()->clear();
    center()->setInboxOpen(false);
}

void MessageCenterWiringTest::documentErrorsReachTheCenter()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    SessionModel sessions;
    span::application::SettingsStore state;
    state.setSessionModel(&sessions);
    state.setDatabasePathForTesting(dir.filePath(QStringLiteral("state.db")));
    QVERIFY(state.open());

    DocumentManager documents;
    QVERIFY(documents.initialize(state.databasePath()));
    span::application::connectDocumentMessages(&documents);

    const QString binary = dir.filePath(QStringLiteral("blob.bin"));
    writeBytes(binary, QByteArray("a\0b", 3));
    QVERIFY(!documents.openFile(binary));
    QTRY_COMPARE(center()->count(), 1);
    QCOMPARE(kindAt(0), QStringLiteral("error"));
    QCOMPARE(center()->unreadErrorCount(), 1);
}

void MessageCenterWiringTest::fileTreeOperationsPostSuccess()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    FileTreeModel files;
    span::application::connectFileTreeMessages(&files, &files);

    files.setRootPath(dir.path());
    files.createFile(dir.path(), QStringLiteral("created.txt"));
    QVERIFY(QFileInfo::exists(dir.filePath(QStringLiteral("created.txt"))));
    QTRY_COMPARE(center()->count(), 1);
    QCOMPARE(kindAt(0), QStringLiteral("success"));
}

void MessageCenterWiringTest::fileTreeRefusalsPostErrors()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    FileTreeModel files;
    span::application::connectFileTreeMessages(&files, &files);

    files.setRootPath(dir.path());
    files.createFile(dir.path(), QStringLiteral(".."));
    QTRY_COMPARE(center()->count(), 1);
    QCOMPARE(kindAt(0), QStringLiteral("error"));
}

void MessageCenterWiringTest::gitRefusalsPostErrors()
{
    GitRepositoryService git;
    span::application::connectGitMessages(&git, &git);

    git.selectHistoryCommit(QStringLiteral("--all"));
    QVERIFY(!git.errorMessage().isEmpty());
    QTRY_COMPARE(center()->count(), 1);
    QCOMPARE(kindAt(0), QStringLiteral("error"));
}

void MessageCenterWiringTest::remoteFileRefusalsPostErrors()
{
    // Refused before any connection: no ssh process is started.
    span::remote::RemoteFileModel files;
    span::application::connectRemoteFileMessages(&files, &files);

    files.createFolder(QStringLiteral(".."));
    QVERIFY(!files.errorMessage().isEmpty());
    QTRY_COMPARE(center()->count(), 1);
    QCOMPARE(kindAt(0), QStringLiteral("error"));
}

void MessageCenterWiringTest::clearingAnErrorDoesNotPostAgain()
{
    GitRepositoryService git;
    span::application::connectGitMessages(&git, &git);

    git.selectHistoryCommit(QStringLiteral("--all"));
    QTRY_COMPARE(center()->count(), 1);
    git.clearError();
    QVERIFY(git.errorMessage().isEmpty());
    QCoreApplication::processEvents();
    QCOMPARE(center()->count(), 1);
}

void MessageCenterWiringTest::connectionEndsWithItsLifetimeOwner()
{
    // The runtime ties each model's messages to its project context; a
    // closed project must not keep posting through a model that outlives it.
    GitRepositoryService git;
    auto context = std::make_unique<QObject>();
    span::application::connectGitMessages(&git, context.get());
    context.reset();

    git.selectHistoryCommit(QStringLiteral("--all"));
    QVERIFY(!git.errorMessage().isEmpty());
    QCoreApplication::processEvents();
    QCOMPARE(center()->count(), 0);
}

void MessageCenterWiringTest::terminalProgressAndNotificationsReachTheHost()
{
    // A child reports taskbar progress (OSC 9;4), turns it to error, clears it
    // and sends an OSC 777 notification: the task ends as failed and the
    // notification becomes a message titled by the child.
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QChar esc(0x1b);
    const QChar bel(0x07);
    const QString command = QStringLiteral("cmd.exe /d /c \"echo %1]9;4;1;40%2& echo %1]9;4;2;40%2"
                                           "& echo %1]9;4;0;0%2& echo %1]777;notify;Build;done%2\"")
                                .arg(esc, bel);
    span::terminal::TerminalManager terminals;
    span::application::TaskProgressService tasks;
    span::application::connectTerminalHostEvents(&terminals, &tasks);
    QVERIFY(terminals.createSession(QStringLiteral("child"), dir.path(),
                                    {{QStringLiteral("commandLine"), command}}));
    terminals.setActiveSession(QStringLiteral("child"));

    QTRY_COMPARE_WITH_TIMEOUT(center()->count(), 1, 10000);
    QCOMPARE(center()->data(center()->index(0), MessageCenter::TitleRole).toString(),
             QStringLiteral("Build"));
    QCOMPARE(center()->data(center()->index(0), MessageCenter::BodyRole).toString(),
             QStringLiteral("done"));
    QTRY_COMPARE_WITH_TIMEOUT(tasks.rowCount(), 1, 5000);
    QTRY_COMPARE_WITH_TIMEOUT(
        tasks.data(tasks.index(0), span::application::TaskProgressService::OutcomeRole).toInt(),
        int(span::application::TaskProgressService::Failed), 5000);
}

int main(int argc, char **argv)
{
    GuiApplication app(argc, argv);
    MessageCenter center;
    MessageCenterWiringTest test;
    return QTest::qExec(&test, argc, argv);
}

#include "message_center_wiring_test.moc"
