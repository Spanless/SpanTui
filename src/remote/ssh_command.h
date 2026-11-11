#pragma once

#include <QString>
#include <QStringList>

namespace span::remote {

// One saved SSH destination. Every value reaches ssh.exe/sftp.exe as its own
// argument after validateEndpoint(); nothing is spliced into a shell string.
struct SshEndpoint {
    QString host;
    int port = 22;
    QString user;
    // Local private key file; empty lets ssh use the agent and its defaults.
    QString identityFile;
};

enum class EndpointError {
    None,
    Host,
    Port,
    User,
    IdentityFile,
};

// Rejects values that ssh would parse as options or that cannot be a host,
// user or port (leading '-', whitespace, quotes, control characters, '%'),
// and a private key path that is not an existing file.
EndpointError checkEndpoint(const SshEndpoint &endpoint);

// The OpenSSH client executables the tool registry chose (see
// core/external_tools.h). Empty while the tool is unavailable.
QString sshExecutable();
QString sftpExecutable();

// Interactive login shell inside the terminal's pseudo console.
QStringList sshTerminalArguments(const SshEndpoint &endpoint);
// Pipe-driven POSIX shell without a TTY for the remote file channel.
QStringList sshShellArguments(const SshEndpoint &endpoint);
// sftp reading its batch from stdin. BatchMode=no precedes -b: sftp turns
// -b into "-oBatchMode yes" and ssh keeps the first value it sees, so the
// askpass bridge stays usable for password logins. An IPv6 host goes in
// brackets: sftp splits its destination as [user@]host[:path].
QStringList sftpBatchArguments(const SshEndpoint &endpoint);

// CreateProcess command line with MSVC argv quoting.
QString windowsCommandLine(const QString &program, const QStringList &arguments);
// Single-quoted POSIX shell word.
QString shellQuote(const QString &value);
// Double-quoted sftp batch word; only " and \ are escaped. sftp itself
// protects * ? [ inside quotes from globbing (makeargv), so a backslash
// before them would become part of the name.
QString sftpQuote(const QString &value);

} // namespace span::remote
