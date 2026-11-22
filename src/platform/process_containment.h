#pragma once

#include <QString>
#include <QStringList>

#include <QtGlobal>

namespace span::platform::process {

// Ties helper processes to SpanTui's lifetime where the platform can.
//   Windows: a process-wide job (kill-on-close, explicit breakaway only). Each
//   ConPTY session still owns its own job, so failure is non-fatal. Shell
//   identity stays owned by the installer/package; this helper must not assign
//   an AppUserModelID that shortcuts do not own.
//   macOS: there is no job object; install does nothing, the job queries
//   answer false, and both start functions start a detached process.
// Call once from the product entrypoint.
void installApplicationContainment();

bool applicationJobActive();
bool applicationJobContainsProcess(quint32 processId);

// Starts a process that belongs to this application: on Windows it keeps this
// process as parent and joins the application job before it runs. Returns the
// process id, 0 on failure with the reason in `errorMessage`.
quint32 startOwnedProcess(const QString &executable, const QStringList &arguments, const QString &workingDirectory,
                          QString *errorMessage = nullptr);

// Starts a process that survives this one: on Windows it breaks away from the
// application job. Use only for programs the user keeps after SpanTui exits:
// the update installer and external editors opened on a project. In-app
// terminals and tools stay on startOwnedProcess.
quint32 startUnaffiliatedProcess(const QString &executable, const QStringList &arguments,
                                 const QString &workingDirectory, QString *errorMessage = nullptr);

} // namespace span::platform::process
