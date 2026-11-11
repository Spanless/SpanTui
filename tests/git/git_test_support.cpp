#include "git_test_support.h"

#include <QFile>
#include <QFileInfo>
#include <QProcess>

namespace span::tests::git {

CommandResult run(const QString &directory, const QStringList &arguments, int timeoutMs)
{
    QProcess process;
    QStringList args{QStringLiteral("-C"), directory};
    args.append(arguments);
    process.start(QStringLiteral("git"), args);
    CommandResult result;
    result.started = process.waitForStarted(timeoutMs);
    if (!result.started) {
        result.stderrBytes = process.errorString().toUtf8();
        return result;
    }
    result.finished = process.waitForFinished(timeoutMs);
    if (!result.finished) {
        process.kill();
        process.waitForFinished();
    }
    result.exitCode = process.exitCode();
    result.stdoutBytes = process.readAllStandardOutput();
    result.stderrBytes = process.readAllStandardError();
    return result;
}

bool initializeRepository(const QString &directory)
{
    if (!run(directory, {QStringLiteral("init"), QStringLiteral("-b"),
                         QStringLiteral("main")}).succeeded()) {
        return false;
    }
    return run(directory, {QStringLiteral("config"), QStringLiteral("user.name"),
                           QStringLiteral("Span Test")}).succeeded()
        && run(directory, {QStringLiteral("config"), QStringLiteral("user.email"),
                           QStringLiteral("span@example.com")}).succeeded()
        && run(directory, {QStringLiteral("config"), QStringLiteral("commit.gpgsign"),
                           QStringLiteral("false")}).succeeded()
        && run(directory, {QStringLiteral("config"), QStringLiteral("core.autocrlf"),
                           QStringLiteral("false")}).succeeded();
}

bool importCommits(const QString &directory, const QList<ImportedCommit> &commits)
{
    QByteArray stream;
    qint64 time = 1700000000;
    for (const ImportedCommit &commit : commits) {
        const QByteArray subject = commit.subject.toUtf8();
        stream += "commit refs/heads/main\ncommitter Span Test <span@example.com> "
            + QByteArray::number(time++) + " +0000\n";
        stream += "data " + QByteArray::number(subject.size()) + '\n' + subject + '\n';
        stream += "M 100644 inline " + commit.path.toUtf8() + '\n';
        stream += "data " + QByteArray::number(commit.contents.size()) + '\n' + commit.contents
            + '\n';
    }
    QProcess process;
    process.start(QStringLiteral("git"),
                  {QStringLiteral("-C"), directory, QStringLiteral("fast-import"),
                   QStringLiteral("--quiet")});
    if (!process.waitForStarted(10000))
        return false;
    process.write(stream);
    process.closeWriteChannel();
    if (!process.waitForFinished(30000) || process.exitStatus() != QProcess::NormalExit
        || process.exitCode() != 0) {
        return false;
    }
    return run(directory, {QStringLiteral("reset"), QStringLiteral("--hard"),
                           QStringLiteral("--quiet")}).succeeded();
}

bool writeFile(const QString &path, const QByteArray &contents)
{
    QFile file(path);
    return file.open(QIODevice::WriteOnly | QIODevice::Truncate)
        && file.write(contents) == contents.size();
}

QString headSubject(const QString &directory)
{
    return QString::fromUtf8(run(directory, {QStringLiteral("log"), QStringLiteral("-1"),
                                             QStringLiteral("--format=%s")})
                                 .stdoutBytes)
        .trimmed();
}

QString currentBranch(const QString &directory)
{
    return QString::fromUtf8(run(directory, {QStringLiteral("branch"), QStringLiteral("--show-current")})
                                 .stdoutBytes)
        .trimmed();
}

}
