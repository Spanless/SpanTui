#include "platform/process_containment.h"

#include <QFileInfo>
#include <QProcess>

namespace span::platform::process {

namespace {

// macOS has no job object, so owned and unaffiliated processes start alike:
// detached, reparented to launchd, never left as a zombie of this process.
quint32 startDetached(const QString &executable, const QStringList &arguments, const QString &workingDirectory,
                      QString *errorMessage)
{
    const QFileInfo info(executable);
    if (!info.exists() || !info.isFile()) {
        if (errorMessage) *errorMessage = QStringLiteral("Executable is missing: %1").arg(executable);
        return 0;
    }
    const QString directory = workingDirectory.trimmed();
    if (!directory.isEmpty() && !QFileInfo(directory).isDir()) {
        if (errorMessage) {
            *errorMessage = QStringLiteral("Working directory is missing or not a directory: %1").arg(directory);
        }
        return 0;
    }
    QProcess process;
    process.setProgram(info.absoluteFilePath());
    process.setArguments(arguments);
    process.setWorkingDirectory(directory);
    qint64 pid = 0;
    if (!process.startDetached(&pid) || pid <= 0) {
        if (errorMessage) *errorMessage = QStringLiteral("Could not start process: %1").arg(process.errorString());
        return 0;
    }
    return quint32(pid);
}

} // namespace

void installApplicationContainment() {}

bool applicationJobActive()
{
    return false;
}

bool applicationJobContainsProcess(quint32)
{
    return false;
}

quint32 startOwnedProcess(const QString &executable, const QStringList &arguments, const QString &workingDirectory,
                          QString *errorMessage)
{
    return startDetached(executable, arguments, workingDirectory, errorMessage);
}

quint32 startUnaffiliatedProcess(const QString &executable, const QStringList &arguments,
                                 const QString &workingDirectory, QString *errorMessage)
{
    return startDetached(executable, arguments, workingDirectory, errorMessage);
}

} // namespace span::platform::process
