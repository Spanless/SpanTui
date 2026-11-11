#include "platform/diagnostics/launch_logging.h"
#include "core/core_constants.h"
#include "core/product_paths.h"

#include "platform/windows/crash_handler.h"
#include "platform/windows/native_dialog.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QLocale>
#include <QMutex>
#include <QMutexLocker>
#include <QTextStream>
#include <QTranslator>

#include <atomic>
#include <cstdio>
#include <cstdlib>

namespace span::platform::diagnostics {
namespace {

constexpr int kMaxProblems = 6;
constexpr int kMaxProblemLength = 500;

QFile *g_launchLog = nullptr;
QMutex g_problemsMutex;
QStringList g_problems;
std::atomic_bool g_dialogsEnabled{false};

// Dialog wording is resolved once on the GUI thread at install time: the
// fatal path can run on any thread, where installing a translator is not
// allowed. Before preferences are readable the user's language choice is
// unknown, so the system UI language decides.
class LaunchFailureText {
    Q_DECLARE_TR_FUNCTIONS(LaunchFailureText)

public:
    QString startTitle;
    QString startIntro;
    QString fatalTitle;
    QString fatalIntro;
    QString noReason;
    QString logLine;
    QString logMissingLine;
    QString revealQuestion;

    void load()
    {
        QTranslator translator;
        bool installed = false;
        const QStringList languages = QLocale::system().uiLanguages();
        if (!languages.isEmpty()
            && languages.first().startsWith(QLatin1String("zh"))
            && translator.load(QStringLiteral(":/i18n/SpanTui_zh_CN.qm"))) {
            installed = QCoreApplication::installTranslator(&translator);
        }
        startTitle = tr("SpanTui could not start");
        startIntro = tr("SpanTui could not start. Recent problems:");
        fatalTitle = tr("SpanTui stopped");
        fatalIntro = tr("SpanTui hit an unrecoverable error and has to close. Recent problems:");
        noReason = tr("No reason was recorded.");
        logLine = tr("Launch log: %1");
        logMissingLine = tr("The launch log could not be written: %1");
        revealQuestion = tr("Open the folder that contains the log?");
        if (installed) {
            QCoreApplication::removeTranslator(&translator);
        }
    }
};

LaunchFailureText g_text;

void recordProblem(QtMsgType type, const QString &message)
{
    if (type != QtWarningMsg && type != QtCriticalMsg && type != QtFatalMsg) {
        return;
    }
    QString text = message.trimmed();
    if (text.isEmpty()) {
        return;
    }
    if (text.size() > kMaxProblemLength) {
        text = text.left(kMaxProblemLength) + QStringLiteral("…");
    }
    const QMutexLocker lock(&g_problemsMutex);
    g_problems.push_back(text);
    while (g_problems.size() > kMaxProblems) {
        g_problems.pop_front();
    }
}

void showFailureDialog(LaunchFailure kind)
{
    if (!g_dialogsEnabled.load()) {
        return;
    }
    const bool fatal = kind == LaunchFailure::Fatal;
    QString title = fatal ? g_text.fatalTitle : g_text.startTitle;
    if (title.isEmpty()) {
        title = span::core::kProductName;
    }
    QStringList body;
    body << (fatal ? g_text.fatalIntro : g_text.startIntro);
    const QStringList problems = recentLaunchProblems();
    if (problems.isEmpty()) {
        body << g_text.noReason;
    } else {
        for (const QString &problem : problems) {
            body << QStringLiteral("• ") + problem;
        }
    }
    const QString logPath =
        QDir::toNativeSeparators(span::core::ProductPaths::launchLogPath());
    const bool logWritten = g_launchLog && g_launchLog->isOpen();
    body << QString()
         << (logWritten ? g_text.logLine : g_text.logMissingLine).arg(logPath);
    if (!logWritten) {
        dialogs::showError(title, body.join(QLatin1Char('\n')));
        return;
    }
    body << QString() << g_text.revealQuestion;
    if (dialogs::askError(title, body.join(QLatin1Char('\n')))) {
        dialogs::revealFolder(QFileInfo(logPath).absolutePath());
    }
}

void launchMessageHandler(QtMsgType type,
                          const QMessageLogContext &context,
                          const QString &message)
{
    const QString line = qFormatLogMessage(type, context, message);
    fprintf(stderr, "%s\n", qPrintable(line));
    fflush(stderr);
    if (g_launchLog && g_launchLog->isOpen()) {
        QTextStream stream(g_launchLog);
        stream << line << '\n';
        stream.flush();
    }
    recordProblem(type, message);
    crash_handler::noteMessage(type, line);
    if (type == QtFatalMsg) {
        crash_handler::reportFatal("QtFatalMsg");
        showFailureDialog(LaunchFailure::Fatal);
        abort();
    }
}

} // namespace

void installLaunchLogging()
{
    g_text.load();
    const QString path = span::core::ProductPaths::launchLogPath();
    QDir().mkpath(QFileInfo(path).absolutePath());
    auto *file = new QFile(path);
    // Without a writable log the handler still runs: stderr and the failure
    // dialog are then the only places a launch problem can surface.
    if (file->open(QIODevice::WriteOnly | QIODevice::Truncate
                   | QIODevice::Text)) {
        g_launchLog = file;
    } else {
        delete file;
    }
    qInstallMessageHandler(launchMessageHandler);
    if (!g_launchLog) {
        qWarning().noquote() << "SpanTui launch log could not be written:"
                             << QDir::toNativeSeparators(path);
        return;
    }
    qInfo().noquote() << "SpanTui product data root:"
                      << span::core::ProductPaths::root();
    qInfo().noquote() << "SpanTui launch log:" << g_launchLog->fileName();
    if (!crash_handler::latestCrashLogPath().isEmpty()) {
        qInfo().noquote()
            << "SpanTui crash log:"
            << crash_handler::latestCrashLogPath();
    }
}

QStringList recentLaunchProblems()
{
    const QMutexLocker lock(&g_problemsMutex);
    return g_problems;
}

void setLaunchFailureDialogsEnabled(bool enabled)
{
    g_dialogsEnabled.store(enabled);
}

void reportLaunchFailure(LaunchFailure kind)
{
    showFailureDialog(kind);
}

} // namespace span::platform::diagnostics
