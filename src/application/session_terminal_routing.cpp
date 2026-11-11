#include "application/session_terminal_routing.h"

#include "application/shell_profile_registry.h"
#include "remote/remote_host_registry.h"
#include "terminal/api/terminal_manager.h"
#include "workspace/scope.h"
#include "workspace/session_model.h"

namespace span::application {

using span::remote::RemoteHostRegistry;
using span::terminal::TerminalManager;

void connectSessionTerminals(SessionModel *sessions,
                             TerminalManager *terminals,
                             RemoteHostRegistry *remoteHosts,
                             ShellProfileRegistry *shells)
{
    const auto synchronize = [sessions, terminals, remoteHosts, shells]() {
        QVector<terminal::TerminalSessionSpec> specs;
        for (const SessionRecord &record : sessions->records()) {
            terminal::TerminalSessionSpec spec;
            spec.id = record.id;
            spec.cwd = record.cwd;
            const workspace::Scope scope = workspace::Scope::fromKey(record.projectPath);
            if (scope.isRemoteHost()) {
                const QString hostId = scope.hostId();
                // Kept only because the host list could not be read: with
                // no ssh command it would start a local shell instead.
                if (!remoteHosts->contains(hostId)) {
                    continue;
                }
                spec.commandLine = remoteHosts->terminalCommandLine(hostId);
                spec.environment = remoteHosts->terminalEnvironment(hostId);
            } else {
                spec.commandLine = sessions->takeInitialCommandLine(record.id);
                // The shell is fixed when the terminal is created: profile
                // edits reach new terminals, not running or restarted ones.
                if (spec.commandLine.isEmpty() && !terminals->session(record.id)) {
                    spec.commandLine = shells->commandLine(sessions->shellProfileForSession(record.id));
                }
            }
            specs.push_back(spec);
        }
        terminals->synchronize(specs);
        // The catalog flags one last-active row per project. Focus the
        // globally selected catalog entry after catalog rebuild.
        terminals->setActiveSession(sessions->activeSessionId());
    };
    QObject::connect(sessions, &SessionModel::sessionsChanged, terminals, synchronize);
    // An edited host restarts its existing terminals at the new endpoint.
    QObject::connect(remoteHosts, &RemoteHostRegistry::hostsChanged, terminals, synchronize);
    QObject::connect(sessions, &SessionModel::activeSessionIdChanged, terminals,
                     [sessions, terminals]() {
                         terminals->setActiveSession(sessions->activeSessionId());
                     });
    synchronize();
}

} // namespace span::application
