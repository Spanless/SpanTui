#include "terminal/api/terminal_manager.h"
#include "terminal/core/terminal_color.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>
#include <QTest>

using namespace span::terminal;

namespace {

TerminalSessionSpec spec(const QString &id, const QString &cwd, const QString &command = {})
{
    TerminalSessionSpec result;
    result.id = id;
    result.cwd = cwd;
    result.commandLine = command;
    return result;
}

// The host list with one session; an empty command lists none.
QVector<TerminalSessionSpec> listedSession(const QString &id, const QString &cwd, const QString &command)
{
    return {spec(id, cwd, command)};
}

} // namespace

class TerminalManagerTest final : public QObject {
    Q_OBJECT

private slots:
    void keepsBackgroundSessionsRunning();
    void synchronizationDoesNotChooseFocus();
    void repeatedRoutingDoesNotRestartExitedSession();
    void synchronizationForwardsInitialCommand();
    void restartUsesLatestListedCommand();
    void appliesSurfaceColorsBeforeChildStarts();
};

void TerminalManagerTest::keepsBackgroundSessionsRunning() {
    TerminalManager manager;
    manager.synchronize({
        spec(QStringLiteral("first"), QDir::homePath()),
        spec(QStringLiteral("second"), QDir::homePath()),
    });
    QVERIFY(manager.session(QStringLiteral("first")));
    QVERIFY(manager.session(QStringLiteral("second")));
    QCOMPARE(manager.activeSession(), nullptr);
    manager.setActiveSession(QStringLiteral("first"));
    QTRY_VERIFY(manager.session(QStringLiteral("first"))->running());
    QVERIFY(!manager.session(QStringLiteral("second"))->running());

    manager.setActiveSession(QStringLiteral("second"));
    QTRY_VERIFY(manager.session(QStringLiteral("second"))->running());
    // Switching focus must not kill the previous session's process.
    QVERIFY(manager.session(QStringLiteral("first"))->running());
    QCOMPARE(manager.activeSession(), manager.session(QStringLiteral("second")));
}

void TerminalManagerTest::synchronizationDoesNotChooseFocus() {
    TerminalManager manager;
    const QVector<TerminalSessionSpec> initial{
        spec(QStringLiteral("first"), QDir::homePath()),
        spec(QStringLiteral("second"), QDir::homePath()),
    };

    manager.synchronize(initial);
    QCOMPARE(manager.activeSession(), nullptr);

    manager.setActiveSession(QStringLiteral("second"));
    QCOMPARE(manager.activeSession(), manager.session(QStringLiteral("second")));

    QVector<TerminalSessionSpec> rebuilt = initial;
    rebuilt.push_back(spec(QStringLiteral("third"), QDir::homePath()));
    manager.synchronize(rebuilt);
    QCOMPARE(manager.activeSession(), manager.session(QStringLiteral("second")));
}

void TerminalManagerTest::repeatedRoutingDoesNotRestartExitedSession() {
    TerminalManager manager;
    manager.synchronize({
        spec(QStringLiteral("session"), QDir::homePath()),
        spec(QStringLiteral("other"), QDir::homePath()),
    });
    manager.setActiveSession(QStringLiteral("session"));
    QTRY_VERIFY(manager.activeSession()->running());
    QVERIFY(manager.activeSession()->launchAttempted());

    // Idempotent re-route while already focused must not restart.
    manager.activeSession()->stop();
    QVERIFY(!manager.activeSession()->running());
    manager.setActiveSession(QStringLiteral("session"));
    QVERIFY(!manager.activeSession()->running());

    // Leaving and returning to an exited session must also stay stopped so
    // script commandLines are never re-run by tab focus alone.
    manager.setActiveSession(QStringLiteral("other"));
    QTRY_VERIFY(manager.session(QStringLiteral("other"))->running());
    manager.setActiveSession(QStringLiteral("session"));
    QVERIFY(!manager.session(QStringLiteral("session"))->running());
    QCOMPARE(manager.activeSession(), manager.session(QStringLiteral("session")));

    manager.restartSession(QStringLiteral("session"));
    QTRY_VERIFY(manager.activeSession()->running());
    manager.activeSession()->stop();

    // Explicit restart while another tab is focused still starts the target.
    manager.setActiveSession(QStringLiteral("other"));
    QTRY_VERIFY(manager.session(QStringLiteral("other"))->running());
    manager.session(QStringLiteral("session"))->stop();
    manager.restartSession(QStringLiteral("session"));
    QCOMPARE(manager.activeSession(), manager.session(QStringLiteral("session")));
    QTRY_VERIFY(manager.session(QStringLiteral("session"))->running());
    manager.activeSession()->stop();
}

void TerminalManagerTest::synchronizationForwardsInitialCommand() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString marker = QDir::toNativeSeparators(
        dir.filePath(QStringLiteral("command-started.txt")));
    QString literalPath = marker;
    literalPath.replace(QLatin1Char('\''), QStringLiteral("''"));
    const QString command = QStringLiteral(
        "powershell.exe -NoLogo -NoProfile -Command \"Set-Content "
        "-LiteralPath '%1' -Value started\"")
        .arg(literalPath);

    TerminalManager manager;
    manager.synchronize({spec(QStringLiteral("command-session"), dir.path(), command)});
    manager.setActiveSession(QStringLiteral("command-session"));

    QTRY_VERIFY_WITH_TIMEOUT(QFileInfo::exists(marker), 10000);
}

void TerminalManagerTest::restartUsesLatestListedCommand() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const auto markerCommand = [&](const QString &name) {
        QString path = QDir::toNativeSeparators(dir.filePath(name));
        path.replace(QLatin1Char('\''), QStringLiteral("''"));
        return QStringLiteral("powershell.exe -NoLogo -NoProfile -Command \"Set-Content "
                              "-LiteralPath '%1' -Value started\"")
            .arg(path);
    };
    const QString id = QStringLiteral("remote-session");
    const auto listed = [&](const QString &command) { return listedSession(id, dir.path(), command); };

    TerminalManager manager;
    manager.synchronize(listed(markerCommand(QStringLiteral("a.txt"))));
    manager.setActiveSession(id);
    QTRY_VERIFY_WITH_TIMEOUT(QFileInfo::exists(dir.filePath(QStringLiteral("a.txt"))), 10000);
    QTRY_VERIFY_WITH_TIMEOUT(!manager.session(id)->running(), 10000);

    // The host was edited: the exited session restarts at the new command.
    // A later listing without a command (a consumed one-shot) keeps it.
    manager.synchronize(listed(markerCommand(QStringLiteral("b.txt"))));
    manager.synchronize(listed({}));
    QVERIFY(!manager.session(id)->running());
    QVERIFY(QFile::remove(dir.filePath(QStringLiteral("a.txt"))));
    manager.restartSession(id);
    QTRY_VERIFY_WITH_TIMEOUT(QFileInfo::exists(dir.filePath(QStringLiteral("b.txt"))), 10000);
    QVERIFY(!QFileInfo::exists(dir.filePath(QStringLiteral("a.txt"))));
}

void TerminalManagerTest::appliesSurfaceColorsBeforeChildStarts() {
    TerminalManager manager;
    manager.setSurfaceColors(defaultSurfaceForeground(false),
                             defaultSurfaceBackground(false),
                             defaultAnsiPalette(false));
    manager.synchronize({spec(QStringLiteral("light"), QDir::tempPath())});

    TerminalSession *session = manager.session(QStringLiteral("light"));
    QVERIFY(session);
    QCOMPARE(session->colorFgBg(), QStringLiteral("0;15"));

    manager.setSurfaceColors(defaultSurfaceForeground(true),
                             defaultSurfaceBackground(true),
                             defaultAnsiPalette(true));
    QCOMPARE(session->colorFgBg(), QStringLiteral("15;0"));
}

QTEST_MAIN(TerminalManagerTest)
#include "terminal_manager_test.moc"
