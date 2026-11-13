#pragma once

#include <QString>
#include <QStringList>

#include <QtGlobal>

namespace span::platform::process {

// Process-wide Windows job (kill-on-close, explicit breakaway only). Call once
// from the product entrypoint. Failure is non-fatal: each ConPTY session still
// owns its own job. Shell identity stays owned by the installer/package; this
// runtime helper must not assign an AppUserModelID that shortcuts do not own.
void installApplicationContainment();

bool applicationJobActive();
bool applicationJobContainsProcess(quint32 processId);

// CreateProcess without DETACHED_PROCESS or CREATE_NEW_PROCESS_GROUP so the
// child keeps this process as parent. When the application job is active the
// child is assigned to it before it runs.
quint32 startOwnedProcess(const QString &executable,
                          const QStringList &arguments,
                          const QString &workingDirectory,
                          QString *errorMessage = nullptr);

// CreateProcess with CREATE_BREAKAWAY_FROM_JOB so the child survives this
// process exiting and closing the application job. Use only for programs the
// user keeps after SpanTui exits: the update installer and external editors
// opened on a project. In-app terminals and tools stay on startOwnedProcess.
quint32 startUnaffiliatedProcess(const QString &executable,
                                 const QStringList &arguments,
                                 const QString &workingDirectory,
                                 QString *errorMessage = nullptr);

} // namespace span::platform::process
