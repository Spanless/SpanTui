#include "terminal/shared/terminal_paste.h"
#include "terminal/terminal_constants.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>
#include <QtTest>

using namespace span::terminal;

namespace {

void writeFile(const QString &path, const QByteArray &bytes, const QDateTime &modified)
{
    QFile file(path);
    QVERIFY(file.open(QIODevice::WriteOnly));
    file.write(bytes);
    // Flushed first: closing would write the buffer and stamp the time again.
    QVERIFY(file.flush());
    QVERIFY(file.setFileTime(modified, QFileDevice::FileModificationTime));
}

QByteArray readFile(const QString &path)
{
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
}

} // namespace

class TerminalPasteTest : public QObject {
    Q_OBJECT

private slots:
    void quotingFollowsTheSessionProgram_data();
    void quotingFollowsTheSessionProgram();
    void quotesPathsForEachShell_data();
    void quotesPathsForEachShell();
    void pastesPathsAsOneLineWithTrailingSpace();
    void remoteSessionsGetNoLocalPath();
    void clipboardPrefersFilesThenTextThenImage();
    void expiresOldAndSurplusStagedFiles();
    void stagesPastedImageAndPrunesExpiredFiles();
    void copiesOnlyTransientDroppedFiles();
};

void TerminalPasteTest::quotingFollowsTheSessionProgram_data()
{
    QTest::addColumn<QString>("commandLine");
    QTest::addColumn<int>("quoting");
    QTest::newRow("zsh") << QStringLiteral("/bin/zsh -l") << int(PathQuoting::Posix);
    QTest::newRow("git bash") << QStringLiteral(R"("C:\Program Files\Git\bin\bash.exe" --login)")
                              << int(PathQuoting::Posix);
    QTest::newRow("pwsh") << QStringLiteral(R"("C:\Program Files\PowerShell\7\pwsh.exe" -NoLogo)")
                          << int(PathQuoting::PowerShell);
    QTest::newRow("powershell") << QStringLiteral(R"(C:\Windows\System32\WindowsPowerShell\v1.0\powershell.exe)")
                                << int(PathQuoting::PowerShell);
    QTest::newRow("cmd") << QStringLiteral(R"(C:\Windows\System32\cmd.exe)") << int(PathQuoting::Cmd);
    QTest::newRow("wsl") << QStringLiteral("wsl.exe -d Ubuntu") << int(PathQuoting::Wsl);
    QTest::newRow("ssh") << QStringLiteral("/usr/bin/ssh -t dev@example") << int(PathQuoting::Remote);
    QTest::newRow("ssh.exe") << QStringLiteral(R"("C:\Windows\System32\OpenSSH\ssh.exe" host)")
                             << int(PathQuoting::Remote);
}

void TerminalPasteTest::quotingFollowsTheSessionProgram()
{
    QFETCH(QString, commandLine);
    QFETCH(int, quoting);
    QCOMPARE(int(pathQuotingForCommandLine(commandLine)), quoting);
}

void TerminalPasteTest::quotesPathsForEachShell_data()
{
    QTest::addColumn<QString>("path");
    QTest::addColumn<int>("quoting");
    QTest::addColumn<QString>("expected");
    QTest::newRow("posix plain") << QStringLiteral("/tmp/a-1.txt") << int(PathQuoting::Posix)
                                 << QStringLiteral("/tmp/a-1.txt");
    QTest::newRow("posix space") << QStringLiteral("/tmp/My File.png") << int(PathQuoting::Posix)
                                 << QStringLiteral("'/tmp/My File.png'");
    QTest::newRow("posix quote") << QStringLiteral("/tmp/it's") << int(PathQuoting::Posix)
                                 << QStringLiteral(R"('/tmp/it'\''s')");
    QTest::newRow("posix expansion") << QStringLiteral("/tmp/$HOME*") << int(PathQuoting::Posix)
                                     << QStringLiteral("'/tmp/$HOME*'");
    QTest::newRow("posix unicode") << QStringLiteral("/tmp/截图 1.png") << int(PathQuoting::Posix)
                                   << QStringLiteral("'/tmp/截图 1.png'");
    QTest::newRow("powershell plain") << QStringLiteral(R"(C:\x\y.txt)") << int(PathQuoting::PowerShell)
                                      << QStringLiteral(R"(C:\x\y.txt)");
    QTest::newRow("powershell quote") << QStringLiteral(R"(C:\a b\it's.png)") << int(PathQuoting::PowerShell)
                                      << QStringLiteral(R"('C:\a b\it''s.png')");
    QTest::newRow("powershell comma") << QStringLiteral(R"(C:\a,b.txt)") << int(PathQuoting::PowerShell)
                                      << QStringLiteral(R"('C:\a,b.txt')");
    QTest::newRow("powershell typographic quote") << QStringLiteral("C:\\it\u2019s.png") << int(PathQuoting::PowerShell)
                                                  << QStringLiteral("'C:\\it\u2019\u2019s.png'");
    QTest::newRow("cmd plain") << QStringLiteral(R"(C:\x\y.txt)") << int(PathQuoting::Cmd)
                               << QStringLiteral(R"(C:\x\y.txt)");
    QTest::newRow("cmd space") << QStringLiteral(R"(C:\a b\x&y.png)") << int(PathQuoting::Cmd)
                               << QStringLiteral(R"("C:\a b\x&y.png")");
    QTest::newRow("cmd percent") << QStringLiteral(R"(C:\a%PATH%b.txt)") << int(PathQuoting::Cmd)
                                 << QStringLiteral(R"("C:\a"^%"PATH"^%"b.txt")");
    QTest::newRow("wsl drive") << QStringLiteral(R"(C:\Users\a b\x.png)") << int(PathQuoting::Wsl)
                               << QStringLiteral("'/mnt/c/Users/a b/x.png'");
    QTest::newRow("wsl share") << QStringLiteral(R"(\\wsl$\Ubuntu\home\a\x.png)") << int(PathQuoting::Wsl)
                               << QStringLiteral("/home/a/x.png");
    QTest::newRow("wsl localhost") << QStringLiteral(R"(\\wsl.localhost\Ubuntu\srv)") << int(PathQuoting::Wsl)
                                   << QStringLiteral("/srv");
}

void TerminalPasteTest::quotesPathsForEachShell()
{
    QFETCH(QString, path);
    QFETCH(int, quoting);
    QFETCH(QString, expected);
    QCOMPARE(quotePath(path, PathQuoting(quoting)), expected);
}

void TerminalPasteTest::pastesPathsAsOneLineWithTrailingSpace()
{
    QCOMPARE(pastePaths({QStringLiteral("/a/b.png"), QStringLiteral("/c d/e.txt")}, PathQuoting::Posix),
             QStringLiteral("/a/b.png '/c d/e.txt' "));
    QCOMPARE(pastePaths({}, PathQuoting::Posix), QString());
}

void TerminalPasteTest::remoteSessionsGetNoLocalPath()
{
    QCOMPARE(quotePath(QStringLiteral("/a/b.png"), PathQuoting::Remote), QString());
    QCOMPARE(pastePaths({QStringLiteral("/a/b.png")}, PathQuoting::Remote), QString());
}

void TerminalPasteTest::clipboardPrefersFilesThenTextThenImage()
{
    // Finder: file URL, file name as text, file icon as image.
    QCOMPARE(classifyClipboard(true, true, true), ClipboardPasteKind::Files);
    // A spreadsheet range: text and a picture of it.
    QCOMPARE(classifyClipboard(false, true, true), ClipboardPasteKind::Text);
    // A screenshot.
    QCOMPARE(classifyClipboard(false, false, true), ClipboardPasteKind::Image);
    QCOMPARE(classifyClipboard(false, false, false), ClipboardPasteKind::Nothing);
}

void TerminalPasteTest::expiresOldAndSurplusStagedFiles()
{
    const QDateTime now = QDateTime::fromSecsSinceEpoch(1'800'000'000);
    const QVector<StagedFile> files{
        {QStringLiteral("old"), now.addSecs(-kPasteStagingMaxAgeSeconds - 1)},
        {QStringLiteral("new"), now.addSecs(-10)},
        {QStringLiteral("newer"), now.addSecs(-5)},
        {QStringLiteral("older"), now.addSecs(-20)},
    };
    QCOMPARE(expiredStagedFiles(files, now, kPasteStagingMaxAgeSeconds, 2),
             (QStringList{QStringLiteral("older"), QStringLiteral("old")}));
    QCOMPARE(expiredStagedFiles(files, now, kPasteStagingMaxAgeSeconds, kPasteStagingMaxFiles),
             QStringList{QStringLiteral("old")});
}

void TerminalPasteTest::stagesPastedImageAndPrunesExpiredFiles()
{
    QTemporaryDir staging;
    QVERIFY(staging.isValid());
    const QDateTime now = QDateTime::currentDateTime();
    const QString stale = QDir(staging.path()).filePath(QStringLiteral("stale.png"));
    writeFile(stale, "old", now.addSecs(-kPasteStagingMaxAgeSeconds - 60));

    QString error;
    const QString first = stagePastedImage("PNG-1", staging.path(), now, &error);
    const QString second = stagePastedImage("PNG-2", staging.path(), now, &error);
    QVERIFY2(!first.isEmpty() && !second.isEmpty(), qPrintable(error));
    QVERIFY(first != second);
    QCOMPARE(readFile(first), QByteArray("PNG-1"));
    QCOMPARE(readFile(second), QByteArray("PNG-2"));
    QVERIFY(first.endsWith(QLatin1String(".png")));
    QVERIFY(!QFileInfo::exists(stale));
}

void TerminalPasteTest::copiesOnlyTransientDroppedFiles()
{
    // Tests run with TMPDIR/TEMP set to their own folder, so a temporary
    // directory is the system temp folder here too.
    QTemporaryDir source;
    QTemporaryDir staging;
    QVERIFY(source.isValid() && staging.isValid());
    const QString screenshot = QDir(source.path()).filePath(QStringLiteral("Screenshot 1.png"));
    writeFile(screenshot, "pixels", QDateTime::currentDateTime());
    QVERIFY(isTransientFile(screenshot));

    const QString staged = stageDroppedFile(screenshot, staging.path(), QDateTime::currentDateTime());
    QVERIFY(staged != screenshot);
    QVERIFY(staged.startsWith(staging.path()));
    QVERIFY(staged.endsWith(QLatin1String("Screenshot 1.png")));
    QCOMPARE(readFile(staged), QByteArray("pixels"));

    const QString program = QCoreApplication::applicationFilePath();
    QVERIFY(!isTransientFile(program));
    QCOMPARE(stageDroppedFile(program, staging.path(), QDateTime::currentDateTime()), program);
}

QTEST_GUILESS_MAIN(TerminalPasteTest)
#include "terminal_paste_test.moc"
