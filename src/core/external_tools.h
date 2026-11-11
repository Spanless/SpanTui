#pragma once

#include <QString>
#include <QStringList>

namespace span::core {

// External command-line tools the product drives as child processes.
enum class ExternalTool {
    Git,
    Ssh,
    Sftp,
    Tailscale,
};

// Executable base name; also the key of the persisted path overrides.
QString toolId(ExternalTool tool);

// Published by the application's tool registry; empty means unavailable.
// Unpublished (tests, standalone terminal): the default location, re-probed
// while missing. GUI thread only.
QString toolExecutable(ExternalTool tool);
void setToolExecutable(ExternalTool tool, const QString &path);

// Where the tool is installed when the user configured nothing: the
// directories that usually hold it, then the search path. Empty when absent.
QString defaultToolExecutable(ExternalTool tool);

// Absolute directories of the process PATH followed by the persisted user
// and machine PATH, so a tool installed after launch is found without a
// restart. Relative entries are dropped: a bare program name would otherwise
// resolve against the current directory, which is the folder being opened.
QStringList executableSearchPath();

// First `fileName` in `directories` that is an executable file.
QString findExecutableIn(const QString &fileName, const QStringList &directories);

} // namespace span::core
