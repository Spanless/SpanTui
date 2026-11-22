#pragma once

#include <QByteArray>
#include <QString>

// Operating-system side of the askpass bridge (ssh_askpass.cpp). The helper
// runs before any Qt application object exists, so none of this needs one.
namespace span::remote::askpass_platform {

// The process that started this one; 0 when unknown.
quint32 parentProcessId();

// Parent and executable file name ("sftp.exe", "sftp") of a running process.
bool processParent(quint32 processId, quint32 *parent, QString *executableName);

// The prompt ssh passed as the helper's only argument.
//   Windows: decoded from the UTF-16 command line; `argv` is unused.
//   macOS: argv[1] as UTF-8.
QString promptArgument(int argc, char **argv);

// Sends `request` to the broker's QLocalServer endpoint and reads its reply
// line. Windows: a named pipe; macOS: a Unix domain socket.
bool exchange(const QString &endpoint, const QByteArray &request, QByteArray *reply);

bool writeStandardOutput(const QByteArray &bytes);

} // namespace span::remote::askpass_platform
