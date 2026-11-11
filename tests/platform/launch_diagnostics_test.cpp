#include "core/product_paths.h"
#include "platform/diagnostics/launch_logging.h"
#include "platform/windows/crash_handler.h"

#include <QFile>
#include <QTemporaryDir>
#include <QTest>

using span::core::ProductPaths;

namespace {

QString readAll(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text))
        return {};
    return QString::fromUtf8(file.readAll());
}

// qWarning("  problem <n>  ") for n in [0, count), padded so trimming shows.
void warnNumbered(int count)
{
    for (int i = 0; i < count; ++i) qWarning("  problem %d  ", i);
}

} // namespace

// Launch logging and the crash handler are process-wide: this test owns its
// process, installs both once into an isolated product root, and observes the
// files and the problem list they keep.
class LaunchDiagnosticsTest final : public QObject {
    Q_OBJECT

private slots:
    void initTestCase();
    void launchLogKeepsRecentProblems();
    void fatalReportWritesTheCrashLogOnce();

private:
    QTemporaryDir m_root;
};

void LaunchDiagnosticsTest::initTestCase()
{
    QVERIFY(m_root.isValid());
    ProductPaths::setRootOverride(m_root.path());
    crash_handler::install(m_root.path());
    span::platform::diagnostics::installLaunchLogging();
}

void LaunchDiagnosticsTest::launchLogKeepsRecentProblems()
{
    qDebug("debug is not a problem");
    qInfo("info is not a problem");
    warnNumbered(7);
    qCritical("%s", QByteArray(600, 'x').constData());

    // The six most recent warnings and criticals, trimmed and capped.
    const QStringList problems = span::platform::diagnostics::recentLaunchProblems();
    QCOMPARE(problems.size(), 6);
    QCOMPARE(problems.first(), QStringLiteral("problem 2"));
    QCOMPARE(problems.at(4), QStringLiteral("problem 6"));
    QCOMPARE(problems.last(), QString(500, QLatin1Char('x')) + QStringLiteral("…"));

    const QString log = readAll(ProductPaths::launchLogPath());
    QVERIFY(log.contains(QStringLiteral("info is not a problem")));
    QVERIFY(log.contains(QStringLiteral("problem 6")));
}

void LaunchDiagnosticsTest::fatalReportWritesTheCrashLogOnce()
{
    qWarning("context before the fault");
    crash_handler::reportFatal("launch diagnostics test");
    crash_handler::reportFatal("second report");

    const QString latestPath = m_root.filePath(QStringLiteral("crash-latest.log"));
    QCOMPARE(crash_handler::latestCrashLogPath(), latestPath);
    const QString latest = readAll(latestPath);
    QVERIFY(latest.contains(QStringLiteral("Reason: launch diagnostics test")));
    // Recent log lines reach the report through the launch message handler.
    QVERIFY(latest.contains(QStringLiteral("context before the fault")));
    QVERIFY(latest.contains(QStringLiteral("Stack (CaptureStackBackTrace):")));
    QVERIFY(!latest.contains(QStringLiteral("second report")));

    const QString history = readAll(m_root.filePath(QStringLiteral("crash.log")));
    QVERIFY(history.contains(QStringLiteral("crash_handler installed")));
    QVERIFY(history.contains(QStringLiteral("Reason: launch diagnostics test")));
}

QTEST_GUILESS_MAIN(LaunchDiagnosticsTest)
#include "launch_diagnostics_test.moc"
