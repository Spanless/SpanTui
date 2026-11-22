#pragma once

#include <QString>

namespace span::platform {

// The address QLocalServer and QLocalSocket use for the per-user endpoint
// `name`.
//   Windows: the name itself, a pipe in the session's namespace.
//   macOS: <per-user system temp>/<name>, the folder confstr() reports, not
//   $TMPDIR: every process of the user agrees on it whatever TMPDIR it was
//   started with, and the path stays inside sun_path's 104 bytes.
QString localEndpointAddress(const QString &name);

} // namespace span::platform
