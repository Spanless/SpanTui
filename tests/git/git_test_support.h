#pragma once

#include <QByteArray>
#include <QList>
#include <QString>
#include <QStringList>
#include <QTest>

// QTRY_* sleeps in 50 ms steps, and a git read finishes at an arbitrary point
// inside one: every wait would lose half a step on average. These recheck
// after each event-loop pass (about every 10 ms) and then report like the
// plain QVERIFY/QCOMPARE.
#define GIT_TRY_VERIFY(expr, timeoutMs) \
    QVERIFY2(QTest::qWaitFor([&]() { return bool(expr); }, timeoutMs), #expr)
#define GIT_TRY_COMPARE(actual, expected, timeoutMs) \
    do { \
        (void)QTest::qWaitFor([&]() { return (actual) == (expected); }, timeoutMs); \
        QCOMPARE(actual, expected); \
    } while (false)

namespace span::tests::git {

struct CommandResult {
    bool started = false;
    bool finished = false;
    int exitCode = -1;
    QByteArray stdoutBytes;
    QByteArray stderrBytes;

    bool succeeded() const { return started && finished && exitCode == 0; }
};

CommandResult run(const QString &directory, const QStringList &arguments,
                  int timeoutMs = 10000);
bool initializeRepository(const QString &directory);

struct ImportedCommit {
    QString path;
    QByteArray contents;
    QString subject;
};
// Builds a linear main history on a repository without commits in a single
// git fast-import, then checks it out: one process instead of an add and a
// commit per entry. Each commit writes one file.
bool importCommits(const QString &directory, const QList<ImportedCommit> &commits);
bool writeFile(const QString &path, const QByteArray &contents);
QString headSubject(const QString &directory);
QString currentBranch(const QString &directory);

}
