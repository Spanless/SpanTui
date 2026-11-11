#pragma once

class SessionModel;

namespace span::remote {
class RemoteHostRegistry;
}
namespace span::terminal {
class TerminalManager;
}

namespace span::application {

class ShellProfileRegistry;

// The terminal manager mirrors the session catalog: every catalog change and
// every host edit synchronizes it again, and the catalog's active session is
// the manager's. A remote session launches its host's ssh command; a local
// one the command it was created with, else — only when no terminal exists
// yet — its shell profile. A remote session whose host is unknown (the host
// list could not be read) is not launched. Synchronizes once on connect.
void connectSessionTerminals(SessionModel *sessions, span::terminal::TerminalManager *terminals,
                             span::remote::RemoteHostRegistry *remoteHosts, ShellProfileRegistry *shells);

} // namespace span::application
