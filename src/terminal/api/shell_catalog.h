#pragma once

#include <QString>
#include <QVector>

namespace span::terminal {

// A shell installed on this machine that a terminal session can start.
struct ShellCandidate {
    // Stable across launches: "pwsh", "powershell", "cmd", "git-bash",
    // "wsl:<distribution>". Sessions persist it.
    QString id;
    // Product name, untranslated.
    QString name;
    // Absolute native path.
    QString executable;
    // File whose embedded icon represents the shell (often `executable`).
    QString icon;
    // Arguments used unless the user replaces them.
    QString arguments;
};

// Installed shells in preference order; the first is the fallback default.
// Git Bash is looked up next to `gitExecutable` (empty skips it). Reads only
// the file system and the registry, so any thread may call it.
QVector<ShellCandidate> detectShells(const QString &gitExecutable = {});

// Process command line for `executable` followed by raw `arguments`.
QString shellCommandLine(const QString &executable, const QString &arguments);

} // namespace span::terminal
