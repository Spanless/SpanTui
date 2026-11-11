#include "application/launch_options.h"

#include <QDir>
#include <QFileInfo>

namespace span::application {
namespace {

QString requiredValue(const QStringList &arguments, int *index, const QString &option,
                      QString *error)
{
    if (*index + 1 >= arguments.size()) {
        *error = QStringLiteral("%1 requires a value.").arg(option);
        return {};
    }
    ++*index;
    return arguments.at(*index);
}

QString canonicalDirectory(const QString &path, const QString &option, QString *error)
{
    const QFileInfo info(path);
    if (!info.isAbsolute() || !info.exists() || !info.isDir()) {
        *error = QStringLiteral("%1 must name an existing absolute directory: %2")
                     .arg(option, path);
        return {};
    }
    return QDir::cleanPath(info.canonicalFilePath());
}

} // namespace

LaunchOptions parseLaunchOptions(const QStringList &arguments)
{
    LaunchOptions result;
    QString project;
    QString cwd;
    for (int index = 1; index < arguments.size(); ++index) {
        const QString argument = arguments.at(index);
        // Internal/development flags are consumed separately; ignore them here
        // so public CLI validation still accepts harness invocations.
        if (argument.startsWith(QLatin1String("--workspace-shot"))
            || argument.startsWith(QLatin1String("--boundary-gallery"))
            || argument.startsWith(QLatin1String("--resource-report="))
            || argument == QLatin1String("--headless")
            || argument == QLatin1String("--no-window")
            || argument == QLatin1String("--isolated-workspace")
            || argument == QLatin1String("--conpty=system")
            || argument == QLatin1String("--trace-viewport")
            || argument == QLatin1String("--trace-viewport=0")
            || argument == QLatin1String("--trace-viewport=1")) {
            continue;
        }
        if (argument == QLatin1String("--terminal")) {
            result.terminalMode = true;
        } else if (argument == QLatin1String("--safe-graphics")) {
            result.safeGraphics = true;
        } else if (argument == QLatin1String("--project")) {
            project = requiredValue(arguments, &index, argument, &result.error);
        } else if (argument == QLatin1String("--data-dir")) {
            const QString dataDir =
                requiredValue(arguments, &index, argument, &result.error);
            const QFileInfo info(dataDir);
            if (result.error.isEmpty()
                && (!info.isAbsolute() || (info.exists() && !info.isDir()))) {
                result.error = QStringLiteral(
                    "--data-dir must name an absolute directory path: %1").arg(dataDir);
            }
            result.dataDir = QDir::cleanPath(info.absoluteFilePath());
        } else if (argument == QLatin1String("--cwd")) {
            cwd = requiredValue(arguments, &index, argument, &result.error);
        } else if (argument == QLatin1String("--command")) {
            result.commandLine =
                requiredValue(arguments, &index, argument, &result.error);
        } else if (argument == QLatin1String("--help")
                   || argument == QLatin1String("-h")) {
            result.error = QStringLiteral(
                "Usage: SpanTui [--project <absolute-dir>] | "
                "--terminal --cwd <absolute-dir> [--command <line>]; "
                "add --safe-graphics to render in software for this launch, "
                "--data-dir <absolute-dir> to keep all data in that folder");
            return result;
        } else if (argument.startsWith(QLatin1Char('-'))) {
            result.error = QStringLiteral("Unknown option: %1").arg(argument);
            return result;
        } else {
            result.error = QStringLiteral("Unexpected argument: %1").arg(argument);
            return result;
        }
        if (!result.error.isEmpty()) {
            return result;
        }
    }

    if (result.terminalMode) {
        if (!project.isEmpty()) {
            result.error = QStringLiteral("--project cannot be combined with --terminal.");
            return result;
        }
        if (cwd.isEmpty()) {
            result.error = QStringLiteral("--terminal requires --cwd.");
            return result;
        }
        result.terminalCwd = canonicalDirectory(cwd, QStringLiteral("--cwd"), &result.error);
    } else {
        if (!cwd.isEmpty() || !result.commandLine.isEmpty()) {
            result.error = QStringLiteral("--cwd and --command require --terminal.");
            return result;
        }
        if (!project.isEmpty()) {
            result.projectPath =
                canonicalDirectory(project, QStringLiteral("--project"), &result.error);
        }
    }
    return result;
}

} // namespace span::application
