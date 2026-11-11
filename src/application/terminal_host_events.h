#pragma once

namespace span::terminal {
class TerminalManager;
}

namespace span::application {

class TaskProgressService;

// Routes what terminal children ask of the host into the application:
// OSC 9 / OSC 777 notifications become message-center entries, and OSC 9;4
// progress becomes a task-progress entry when the runtime has that service.
void connectTerminalHostEvents(span::terminal::TerminalManager *terminals,
                               TaskProgressService *progress);

} // namespace span::application
