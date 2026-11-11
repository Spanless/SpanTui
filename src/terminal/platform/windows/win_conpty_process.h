#pragma once

#include "terminal/api/terminal_process.h"

#include <QByteArray>
#include <QColor>
#include <QList>
#include <QString>
#include <QVector>

#include <functional>
#include <memory>

namespace span::terminal {

class WinConPtyProcess final : public TerminalProcess {
public:
    using StartOptions = TerminalProcess::StartOptions;
    using OutputCallback = TerminalProcess::OutputCallback;
    using ExitCallback = TerminalProcess::ExitCallback;

    WinConPtyProcess();
    ~WinConPtyProcess() override;

    WinConPtyProcess(const WinConPtyProcess &) = delete;
    WinConPtyProcess &operator=(const WinConPtyProcess &) = delete;

    static QString defaultShellCommand();
    // True when sessions use the conpty.dll + OpenConsole.exe shipped next to
    // the executable; false when they use the system ConPTY (no bundle, a
    // damaged one, or --conpty=system). Decided once per process, at the
    // first session.
    static bool usesBundledConPty();
    // --conpty=system: skip the bundle for this process to compare the two
    // console hosts. Call before the first session starts.
    static void setPreferSystemConPty(bool prefer);

    bool start(const StartOptions &options,
               OutputCallback outputCallback,
               ExitCallback exitCallback = {},
               QString *errorMessage = nullptr) override;
    bool write(const QByteArray &bytes, TerminalInputKind kind = TerminalInputKind::User) override;
    bool cancelPendingWritesAndInterrupt() override;
    void requestResize(int columns, int rows) override;
    // Bundled ConPTY only: the system one has no clear entry point.
    void clearScreen() override;
    void stop() override;
    bool isRunning() const override;
    qsizetype pendingWriteBytes() const override;
    // Shell process id for the ConPTY child (0 if not running).
    quint32 processId() const override;
    // Every process currently assigned to this session's Windows job, including
    // the console host and foreground descendants. Empty when not running.
    QList<quint32> jobProcessIds() const override;

private:
    struct Impl;
    // Swapped atomically: stop() may race the non-blocking calls.
    std::shared_ptr<Impl> m_impl;
};

} // namespace span::terminal
