#include "application/terminal_resource_targets.h"

#include "terminal/api/terminal_manager.h"
#include "terminal/api/terminal_session.h"

#include <utility>

QVector<ResourceSampleProvider::SessionTarget> terminalResourceTargets(const span::terminal::TerminalManager &terminals)
{
    const QList<span::terminal::TerminalSession *> sessions = terminals.sessions();
    QVector<ResourceSampleProvider::SessionTarget> targets;
    targets.reserve(sessions.size());
    for (const auto *session : sessions) {
        if (!session) continue;
        ResourceSampleProvider::SessionTarget target;
        target.id = session->id();
        target.rootPid = session->processId();
        target.jobPids = session->jobProcessIds();
        target.running = session->running();
        targets.push_back(std::move(target));
    }
    return targets;
}
