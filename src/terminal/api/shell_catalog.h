#pragma once

#include <QString>
#include <QVector>

namespace span::terminal {

// A shell installed on this machine that a terminal session can start.
struct ShellCandidate {
    // Stable across launches. Sessions persist it. Windows: "pwsh",
    // "powershell", "cmd", "git-bash", "wsl:<distribution>"; macOS: the
    // shell's file name ("zsh"), or its path for a second install.
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
// Reads only the file system (and on Windows the registry), so any thread
// may call it.
//   Windows: Git Bash is looked up next to `gitExecutable` (empty skips it).
//   macOS: the login shell ($SHELL), then /etc/shells; each starts as a
//   login shell (-l). `gitExecutable` is unused.
QVector<ShellCandidate> detectShells(const QString &gitExecutable = {});

// Command line for `executable` followed by raw `arguments`; the PTY backend
// splits it the platform's way.
QString shellCommandLine(const QString &executable, const QString &arguments);

} // namespace span::terminal
