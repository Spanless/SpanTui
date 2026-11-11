#pragma once

#include <QtGlobal>

class QString;

// Process-wide crash / fatal diagnostics for the WIN32 GUI app (no console).
// Writes:
//   <logDirectory>/crash-latest.log  — last fault only
//   <logDirectory>/crash.log         — append history
// and keeps a small ring of recent Qt log lines so the report has context.
namespace crash_handler {

// Call once at startup. Installs SEH / terminate / abort handlers and
// prepares log paths. Empty = ProductPaths root.
void install(const QString &logDirectory);

// Feed recent diagnostics into the crash ring buffer (from the Qt message handler).
void noteMessage(QtMsgType type, const QString &line);

// Explicit report (e.g. QtFatal before abort). Safe to call more than once; only
// the first report in a process is written to disk.
void reportFatal(const char *reason);

// Path of the last-written crash-latest.log (empty if install failed).
QString latestCrashLogPath();

} // namespace crash_handler
