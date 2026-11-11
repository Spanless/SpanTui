#pragma once

#include "terminal/api/terminal_input_transaction.h"
#include "terminal/terminal_constants.h"

#include <QByteArray>
#include <QHash>
#include <QList>
#include <QString>

#include <functional>

namespace span::terminal {

// Thread-safe. start() and stop() block on the operating system; every other
// call returns at once, and callbacks arrive on backend threads.
class TerminalProcess {
public:
    struct StartOptions {
        QString commandLine;
        QString workingDirectory;
        int columns = kDefaultColumns;
        int rows = kDefaultRows;
        QString colorFgBg;
        // Variables layered over the inherited block after the terminal
        // identity defaults, so a caller may override those too (remote
        // sessions replace TERM and add the askpass bridge).
        QHash<QString, QString> environment;
    };

    // `endsWrite`: no further bytes were immediately available after this
    // chunk, so it ends one producer write (a cut point for presentation).
    using OutputCallback = std::function<void(const QByteArray &bytes, bool endsWrite)>;
    using ExitCallback = std::function<void(quint32)>;

    virtual ~TerminalProcess() = default;
    virtual bool start(const StartOptions &options,
                       OutputCallback outputCallback,
                       ExitCallback exitCallback,
                       QString *errorMessage) = 0;
    virtual bool write(const QByteArray &bytes,
                       TerminalInputKind kind = TerminalInputKind::User) = 0;
    virtual bool cancelPendingWritesAndInterrupt() = 0;
    // Latest size wins; a failure is logged by the backend.
    virtual void requestResize(int columns, int rows) = 0;
    // The user cleared the terminal: a backend that keeps its own copy of
    // the screen (ConPTY) clears it the same way, keeping the cursor's row
    // as the top row. Backends without one have nothing to do.
    virtual void clearScreen() {}
    virtual void stop() = 0;
    virtual bool isRunning() const = 0;
    virtual qsizetype pendingWriteBytes() const = 0;
    virtual quint32 processId() const = 0;
    virtual QList<quint32> jobProcessIds() const = 0;
};

} // namespace span::terminal
