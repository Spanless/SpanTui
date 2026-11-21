#include "platform/script_launch.h"

#include <QDir>
#include <QFile>
#include <QProcess>
#include <QTemporaryDir>
#include <QTest>

namespace {

bool writeScript(const QString &path, const QByteArray &body)
{
    QFile file(path);
    return file.open(QIODevice::WriteOnly) && file.write(body) == body.size();
}

} // namespace

class ScriptLaunchTest final : public QObject {
    Q_OBJECT

private slots:
    void runsTheScriptThenKeepsAShell_data();
    void runsTheScriptThenKeepsAShell();
    void refusesOtherFilesAndUnquotablePaths();
};

void ScriptLaunchTest::runsTheScriptThenKeepsAShell_data()
{
    QTest::addColumn<QString>("name");
    QTest::newRow("sh") << QStringLiteral("build.sh");
    QTest::newRow("command") << QStringLiteral("Deploy.COMMAND");
    QTest::newRow("zsh") << QStringLiteral("setup.zsh");
    QTest::newRow("metacharacters") << QStringLiteral("it's $HOME & `x`; ok.sh");
}

void ScriptLaunchTest::runsTheScriptThenKeepsAShell()
{
    QFETCH(QString, name);
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString marker = dir.filePath(QStringLiteral("ran.txt"));
    const QString script = dir.filePath(name);
    QVERIFY(writeScript(script, "echo ran > \"$(dirname \"$0\")/ran.txt\"\n"));

    const QString commandLine = span::platform::scriptCommandLine(QDir::toNativeSeparators(script));
    QVERIFY2(!commandLine.isEmpty(), qPrintable(script));
    QStringList arguments = QProcess::splitCommand(commandLine);
    const QString program = arguments.takeFirst();

    // Closed input ends the interactive shell left behind the script.
    QProcess process;
    process.start(program, arguments);
    QVERIFY(process.waitForStarted());
    process.closeWriteChannel();
    QVERIFY(process.waitForFinished(10000));
    QVERIFY2(QFile::exists(marker), process.readAllStandardError().constData());
}

void ScriptLaunchTest::refusesOtherFilesAndUnquotablePaths()
{
    QVERIFY(span::platform::scriptCommandLine(QStringLiteral("/tmp/notes.txt")).isEmpty());
    QVERIFY(span::platform::scriptCommandLine(QStringLiteral("/tmp/noext")).isEmpty());
    QVERIFY(span::platform::scriptCommandLine(QStringLiteral("/tmp/say \"hi\".sh")).isEmpty());
    QVERIFY(span::platform::scriptCommandLine(QStringLiteral("/tmp/two\nlines.sh")).isEmpty());
}

QTEST_GUILESS_MAIN(ScriptLaunchTest)

#include "script_launch_test.moc"
