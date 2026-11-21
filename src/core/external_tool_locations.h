#pragma once

#include "core/external_tools.h"

#include <QString>
#include <QStringList>

namespace span::core {

// Platform facts behind external_tools.

// File name of a program: "git.exe" on Windows, "git" elsewhere.
QString executableFileName(const QString &baseName);

// Search path entries a new login would get beyond the process PATH, so a
// tool installed after launch is found without a restart.
//   Windows: the persisted machine, then user, PATH (registry).
//   macOS: /etc/paths and /etc/paths.d, which path_helper adds for login
//   shells; an application started from Finder inherits none of it.
QStringList persistedSearchPath();

// Folders the tool's usual installers put it in.
QStringList knownToolDirectories(ExternalTool tool);

} // namespace span::core
