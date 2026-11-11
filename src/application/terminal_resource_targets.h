#pragma once

#include "monitoring/resource_monitor.h"

namespace span::terminal {
class TerminalManager;
}

// The sessions of `terminals` as the resource monitor measures them: shell
// process, the processes of the session's job, and whether it is running.
QVector<ResourceSampleProvider::SessionTarget>
terminalResourceTargets(const span::terminal::TerminalManager &terminals);
