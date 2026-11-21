#pragma once

#include "terminal/api/terminal_process.h"

#include <memory>

namespace span::terminal {

// The pseudo terminal of this platform: ConPTY on Windows
// (WinConPtyProcess), a POSIX pty on macOS (MacPtyProcess).
std::unique_ptr<TerminalProcess> createPlatformTerminalProcess();

// --conpty=system: Windows sessions skip the bundled ConPTY for this process
// to compare the two console hosts. No effect elsewhere. Call before the
// first session starts.
void setPreferSystemConsoleHost(bool prefer);

} // namespace span::terminal
