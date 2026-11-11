#include "application/terminal_host_events.h"

#include "application/message_center.h"
#include "application/task_progress.h"
#include "terminal/api/terminal_manager.h"

#include <QCoreApplication>
#include <QHash>

#include <memory>

namespace span::application {

namespace {

using span::terminal::TerminalManager;
using span::terminal::TerminalProgressState;

QString terminalLabel(const TerminalManager *terminals, const QString &sessionId)
{
    const QString title = terminals->sessionTitle(sessionId);
    return title.isEmpty() ? QCoreApplication::translate("TerminalHostEvents", "Terminal") : title;
}

} // namespace

void connectTerminalHostEvents(TerminalManager *terminals, TaskProgressService *progress)
{
    if (!terminals) return;
    QObject::connect(terminals, &TerminalManager::notificationRequested, terminals,
                     [terminals](const QString &sessionId, const QString &title,
                                 const QString &body) {
        auto *center = MessageCenter::instance();
        if (!center) return;
        const QString heading = title.isEmpty() ? terminalLabel(terminals, sessionId) : title;
        if (body.isEmpty()) center->post(MessageKind::Info, heading);
        else center->post(MessageKind::Info, body, heading);
    });
    if (!progress) return;
    // Last reported state per session: start() only on the way in, and an
    // error state still showing when progress ends finishes as failed.
    auto states = std::make_shared<QHash<QString, int>>();
    QObject::connect(terminals, &TerminalManager::progressChanged, progress,
                     [terminals, progress, states](const QString &sessionId) {
        const QString source = QStringLiteral("terminal#") + sessionId;
        const span::terminal::TerminalSession *session = terminals->session(sessionId);
        const auto state = TerminalProgressState(session ? session->progressState() : 0);
        const int previous = states->value(sessionId, int(TerminalProgressState::None));
        if (state == TerminalProgressState::None) {
            states->remove(sessionId);
            if (previous != int(TerminalProgressState::None)) {
                progress->finish(source, previous == int(TerminalProgressState::Error)
                                             ? TaskProgressService::Failed
                                             : TaskProgressService::Succeeded);
            }
            return;
        }
        if (previous == int(TerminalProgressState::None))
            progress->start(source, terminalLabel(terminals, sessionId));
        states->insert(sessionId, int(state));
        const QString detail = state == TerminalProgressState::Error
            ? QCoreApplication::translate("TerminalHostEvents", "Error")
            : state == TerminalProgressState::Paused
                ? QCoreApplication::translate("TerminalHostEvents", "Paused")
                : QString();
        progress->update(source,
                         state == TerminalProgressState::Indeterminate
                             ? -1.0 : session->progressValue() / 100.0,
                         detail);
    });
}

} // namespace span::application
