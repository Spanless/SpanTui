#pragma once

#include "terminal/api/terminal_process.h"

#include <memory>

namespace span::terminal {

// A POSIX pseudo terminal (forkpty). The shell leads its own session with
// the pty as controlling terminal, so the line discipline turns ^C into
// SIGINT for the foreground job.
class MacPtyProcess final : public TerminalProcess {
public:
    MacPtyProcess();
    ~MacPtyProcess() override;

    MacPtyProcess(const MacPtyProcess &) = delete;
    MacPtyProcess &operator=(const MacPtyProcess &) = delete;

    // The user's login shell ($SHELL, else /bin/zsh) as a login shell.
    static QString defaultShellCommand();

    bool start(const StartOptions &options, OutputCallback outputCallback, ExitCallback exitCallback = {},
               QString *errorMessage = nullptr) override;
    bool write(const QByteArray &bytes, TerminalInputKind kind = TerminalInputKind::User) override;
    bool cancelPendingWritesAndInterrupt() override;
    void requestResize(int columns, int rows) override;
    void stop() override;
    bool isRunning() const override;
    qsizetype pendingWriteBytes() const override;
    quint32 processId() const override;
    // No job object: always empty. Resource sampling walks the shell's
    // process tree from processId().
    QList<quint32> jobProcessIds() const override;

private:
    struct Impl;
    // Swapped atomically: stop() may race the non-blocking calls.
    std::shared_ptr<Impl> m_impl;
};

} // namespace span::terminal
