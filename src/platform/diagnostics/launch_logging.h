#pragma once

#include <QString>
#include <QStringList>

namespace span::platform::diagnostics {

enum class LaunchFailure {
    Start, // the process returns a failure exit code before or while starting
    Fatal, // a fatal message is about to abort the process
};

// Also resolves the failure dialog wording, so call it on the GUI thread
// once the application object exists.
void installLaunchLogging();

// Warning, critical and fatal messages seen since launch, oldest first and
// capped to the most recent few. They are the reason a failed start shows
// the user, because the window that would display them never appeared.
QStringList recentLaunchProblems();

// Whether a failed start or a fatal message opens a native dialog. Off for
// headless, automation and offscreen runs, which must never block on a
// window nobody can dismiss.
void setLaunchFailureDialogsEnabled(bool enabled);

// Shows the launch failure dialog (when enabled) with the recent problems
// and the launch log path. Safe before any window exists. The fatal message
// path reports itself; callers use LaunchFailure::Start.
void reportLaunchFailure(LaunchFailure kind);

} // namespace span::platform::diagnostics
