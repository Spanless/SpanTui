#include "application/launch_options.h"
#include "application/runtime/internal_launch_options.h"

#include <QDir>
#include <QFile>
#include <QTemporaryDir>
#include <QtTest>

using span::application::parseLaunchOptions;
using span::application::runtime::parseInternalLaunchOptions;

class LaunchOptionsTest final : public QObject {
    Q_OBJECT

private slots:
    void parsesProjectAndTerminalModes();
    void rejectsAmbiguousOrInvalidPaths();
    void rejectsUnknownPublicOptions();
    void isolatesSmokeWithoutEnablingScreenshotMode();
    void parsesWorkspaceShotOptions();
    void safeGraphicsWorksInBothModes();
    void parsesDataDirectory();
    void parsesDiagnosticFlags();
};

void LaunchOptionsTest::parsesProjectAndTerminalModes()
{
    QTemporaryDir temporary(QStringLiteral("SpanTui 参数 空格-XXXXXX"));
    QVERIFY(temporary.isValid());
    const QString directory = QDir::cleanPath(temporary.path());

    const auto project =
        parseLaunchOptions({QStringLiteral("SpanTui.exe"), QStringLiteral("--project"),
                            directory});
    QVERIFY(project.valid());
    QVERIFY(!project.terminalMode);
    QCOMPARE(project.projectPath, QDir(directory).canonicalPath());

    const auto terminal =
        parseLaunchOptions({QStringLiteral("SpanTui.exe"), QStringLiteral("--terminal"),
                            QStringLiteral("--cwd"), directory,
                            QStringLiteral("--command"),
                            QStringLiteral("printf \"hello world\"")});
    QVERIFY(terminal.valid());
    QVERIFY(terminal.terminalMode);
    QCOMPARE(terminal.terminalCwd, QDir(directory).canonicalPath());
    QCOMPARE(terminal.commandLine, QStringLiteral("printf \"hello world\""));
}

void LaunchOptionsTest::safeGraphicsWorksInBothModes()
{
    const auto plain = parseLaunchOptions({QStringLiteral("SpanTui.exe")});
    QVERIFY(plain.valid());
    QVERIFY(!plain.safeGraphics);

    const auto workspace =
        parseLaunchOptions({QStringLiteral("SpanTui.exe"), QStringLiteral("--safe-graphics")});
    QVERIFY(workspace.valid());
    QVERIFY(workspace.safeGraphics);
    QVERIFY(!workspace.terminalMode);

    const auto terminal =
        parseLaunchOptions({QStringLiteral("SpanTui.exe"), QStringLiteral("--terminal"),
                            QStringLiteral("--cwd"), QDir::tempPath(),
                            QStringLiteral("--safe-graphics")});
    QVERIFY(terminal.valid());
    QVERIFY(terminal.safeGraphics);
    QVERIFY(terminal.terminalMode);
}

void LaunchOptionsTest::rejectsAmbiguousOrInvalidPaths()
{
    const auto relative =
        parseLaunchOptions({QStringLiteral("SpanTui.exe"), QStringLiteral("--project"),
                            QStringLiteral("relative")});
    QVERIFY(!relative.valid());

    const auto missingCwd =
        parseLaunchOptions({QStringLiteral("SpanTui.exe"), QStringLiteral("--terminal")});
    QVERIFY(!missingCwd.valid());

    const auto mixed =
        parseLaunchOptions({QStringLiteral("SpanTui.exe"), QStringLiteral("--terminal"),
                            QStringLiteral("--cwd"), QDir::tempPath(),
                            QStringLiteral("--project"), QDir::tempPath()});
    QVERIFY(!mixed.valid());
}

void LaunchOptionsTest::rejectsUnknownPublicOptions()
{
    const auto typo =
        parseLaunchOptions({QStringLiteral("SpanTui.exe"), QStringLiteral("--projcet"),
                            QDir::tempPath()});
    QVERIFY(!typo.valid());
    QVERIFY(typo.error.contains(QStringLiteral("Unknown option")));

    // Internal harness flags are ignored by the public parser.
    const auto harness =
        parseLaunchOptions({QStringLiteral("SpanTui.exe"), QStringLiteral("--headless")});
    QVERIFY(harness.valid());
    QVERIFY(!harness.terminalMode);
}

void LaunchOptionsTest::isolatesSmokeWithoutEnablingScreenshotMode()
{
    const auto options = parseInternalLaunchOptions(
        {QStringLiteral("SpanTui-test.exe"),
         QStringLiteral("--isolated-workspace")});
    const auto publicOptions = parseLaunchOptions(
        {QStringLiteral("SpanTui-test.exe"),
         QStringLiteral("--isolated-workspace")});

    QVERIFY(publicOptions.valid());
    QVERIFY(!publicOptions.terminalMode);
    QVERIFY(options.isolatedWorkspace);
    QVERIFY(options.isolatedVisualHarness());
    QVERIFY(!options.workspaceShot);
    QVERIFY(options.workspaceShotPath.isEmpty());
}

void LaunchOptionsTest::parsesWorkspaceShotOptions()
{
    const auto options = parseInternalLaunchOptions(
        {QStringLiteral("SpanTui-test.exe"),
         QStringLiteral("--workspace-shot-theme=light"),
         QStringLiteral("--workspace-shot-right-tool=diff"),
         QStringLiteral("--workspace-shot-viewport=1280x720"),
         QStringLiteral("--workspace-shot-terminal")});

#ifdef NDEBUG
    // The visual harness is Debug-only; Release images ignore its flags.
    QVERIFY(!options.workspaceShot);
    QVERIFY(!options.isolatedVisualHarness());
    return;
#endif
    QVERIFY(options.workspaceShot);
    QVERIFY(options.isolatedVisualHarness());
    QCOMPARE(options.workspaceShotTheme, QStringLiteral("light"));
    QCOMPARE(options.workspaceShotRightTool, QStringLiteral("diff"));
    QCOMPARE(options.workspaceShotViewport, QSize(1280, 720));
    QVERIFY(options.workspaceShotTerminal);
}

void LaunchOptionsTest::parsesDataDirectory()
{
    QTemporaryDir root;
    QVERIFY(root.isValid());
    // A missing directory is fine: the product creates its data root.
    const QString missing = QDir(root.path()).filePath(QStringLiteral("data/../data"));
    const auto workspace = parseLaunchOptions(
        {QStringLiteral("SpanTui.exe"), QStringLiteral("--data-dir"), missing});
    QVERIFY2(workspace.valid(), qPrintable(workspace.error));
    QCOMPARE(workspace.dataDir, QDir::cleanPath(QDir(root.path()).filePath(QStringLiteral("data"))));

    const auto terminal = parseLaunchOptions(
        {QStringLiteral("SpanTui.exe"), QStringLiteral("--terminal"), QStringLiteral("--cwd"),
         root.path(), QStringLiteral("--data-dir"), root.path()});
    QVERIFY2(terminal.valid(), qPrintable(terminal.error));
    QVERIFY(terminal.terminalMode);
    QCOMPARE(terminal.dataDir, QDir::cleanPath(root.path()));

    const auto relative = parseLaunchOptions(
        {QStringLiteral("SpanTui.exe"), QStringLiteral("--data-dir"), QStringLiteral(".dev/data")});
    QVERIFY(!relative.valid());
    QVERIFY(relative.error.contains(QStringLiteral("--data-dir")));

    QFile file(QDir(root.path()).filePath(QStringLiteral("file")));
    QVERIFY(file.open(QIODevice::WriteOnly));
    file.close();
    QVERIFY(!parseLaunchOptions({QStringLiteral("SpanTui.exe"), QStringLiteral("--data-dir"),
                                 file.fileName()}).valid());
    QVERIFY(!parseLaunchOptions({QStringLiteral("SpanTui.exe"), QStringLiteral("--data-dir")}).valid());
}

void LaunchOptionsTest::parsesDiagnosticFlags()
{
    const QStringList arguments{QStringLiteral("SpanTui.exe"), QStringLiteral("--conpty=system"),
                                QStringLiteral("--trace-viewport=0")};
    QVERIFY(parseLaunchOptions(arguments).valid());
    const auto options = parseInternalLaunchOptions(arguments);
    QVERIFY(options.systemConPty);
    QCOMPARE(options.traceViewport, std::optional<bool>(false));

    const auto defaults = parseInternalLaunchOptions({QStringLiteral("SpanTui.exe")});
    QVERIFY(!defaults.systemConPty);
    QVERIFY(!defaults.traceViewport.has_value());
    QCOMPARE(parseInternalLaunchOptions({QStringLiteral("SpanTui.exe"),
                                         QStringLiteral("--trace-viewport")})
                 .traceViewport,
             std::optional<bool>(true));
    // Other values are not diagnostics flags and stay unknown options.
    QVERIFY(!parseLaunchOptions({QStringLiteral("SpanTui.exe"), QStringLiteral("--conpty=bundled")})
                 .valid());
}

QTEST_MAIN(LaunchOptionsTest)

#include "launch_options_test.moc"
