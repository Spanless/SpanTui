#pragma once

#include <QString>

namespace span::platform {

// Command line that runs the script at `path` (absolute, native separators)
// in a terminal and then leaves an interactive shell open for inspection.
// Empty when this platform does not run that kind of file (by suffix), or
// when the path holds a double quote or a line break, which no quoting below
// keeps inert.
//   Windows: .bat, .cmd and .ps1 through PowerShell (& '<path>', -NoExit).
//   macOS: .sh and .command through /bin/sh, .bash through bash, .zsh
//   through zsh, inside the login shell ($SHELL, else /bin/zsh), which then
//   stays open.
QString scriptCommandLine(const QString &path);

} // namespace span::platform
