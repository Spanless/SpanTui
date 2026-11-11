#include "stress_tui.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QThread>
#include <QTimer>

#include <atomic>
#include <cstdio>
#include <cstdlib>

#ifdef _WIN32
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <windows.h>
#endif

namespace {

using namespace spantui::stresstui;

std::atomic_bool g_stopRequested = false;

#ifdef _WIN32
BOOL WINAPI consoleControlHandler(DWORD event) {
    if (event == CTRL_C_EVENT || event == CTRL_BREAK_EVENT || event == CTRL_CLOSE_EVENT) {
        g_stopRequested.store(true, std::memory_order_relaxed);
        return TRUE;
    }
    return FALSE;
}
#endif

void prepareConsole() {
#ifdef _WIN32
    SetConsoleOutputCP(CP_UTF8);
    const HANDLE output = GetStdHandle(STD_OUTPUT_HANDLE);
    DWORD mode = 0;
    if (GetConsoleMode(output, &mode)) {
        SetConsoleMode(output, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING);
    }
    SetConsoleCtrlHandler(consoleControlHandler, TRUE);
#endif
}

// One call per WriteStep: the write boundaries are part of what is tested.
void writeAll(const QByteArray &bytes) {
    if (bytes.isEmpty()) return;
#ifdef _WIN32
    const HANDLE output = GetStdHandle(STD_OUTPUT_HANDLE);
    const char *data = bytes.constData();
    DWORD remaining = DWORD(bytes.size());
    while (remaining > 0) {
        DWORD written = 0;
        if (!WriteFile(output, data, remaining, &written, nullptr) || written == 0) {
            g_stopRequested.store(true, std::memory_order_relaxed);
            return;
        }
        data += written;
        remaining -= written;
    }
#else
    std::fwrite(bytes.constData(), 1, size_t(bytes.size()), stdout);
    std::fflush(stdout);
#endif
}

TerminalSize terminalSize() {
#ifdef _WIN32
    CONSOLE_SCREEN_BUFFER_INFO info{};
    if (GetConsoleScreenBufferInfo(GetStdHandle(STD_OUTPUT_HANDLE), &info)) {
        return {std::max(20, int(info.srWindow.Right - info.srWindow.Left + 1)),
                std::max(8, int(info.srWindow.Bottom - info.srWindow.Top + 1))};
    }
#endif
    TerminalSize size;
    if (const char *columns = std::getenv("COLUMNS")) size.columns = std::max(20, std::atoi(columns));
    if (const char *rows = std::getenv("LINES")) size.rows = std::max(8, std::atoi(rows));
    return size;
}

// Runs scenario frames on the Qt event loop. Stalls block that loop on
// purpose: a janky TUI is one whose main thread stops between writes.
class Driver final : public QObject {
public:
    explicit Driver(const Options &options)
        : m_options(options),
          m_random(options.seed),
          m_jank(options, &m_random),
          m_scenario(createScenario(options.scenario, options, &m_random)) {
        m_timer.setSingleShot(true);
        m_timer.setTimerType(Qt::PreciseTimer);
        connect(&m_timer, &QTimer::timeout, this, [this]() { step(); });
    }

    void start() {
        m_clock.start();
        writeAll(m_scenario->enter(terminalSize()));
        m_timer.start(0);
    }

private:
    bool shouldStop() const {
        return g_stopRequested.load(std::memory_order_relaxed)
            || (m_options.durationMs > 0 && m_clock.elapsed() >= m_options.durationMs);
    }

    void step() {
        if (shouldStop()) {
            finish();
            return;
        }
        WritePlan plan = m_scenario->tick(terminalSize());
        if (m_scenario->acceptsJank()) plan = m_jank.apply(plan);
        for (const WriteStep &write : plan) {
            if (g_stopRequested.load(std::memory_order_relaxed)) break;
            writeAll(write.bytes);
            if (write.stallAfterMs > 0) QThread::msleep(ulong(write.stallAfterMs));
        }
        m_timer.start(m_scenario->nextDelayMs());
    }

    void finish() {
        writeAll(m_scenario->leave() + resetSequence() + QByteArrayLiteral("\r\n"));
        QCoreApplication::quit();
    }

    Options m_options;
    QRandomGenerator m_random;
    Jank m_jank;
    std::unique_ptr<ScenarioEngine> m_scenario;
    QTimer m_timer;
    QElapsedTimer m_clock;
};

} // namespace

int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    QStringList arguments = QCoreApplication::arguments();
    arguments.removeFirst();

    Options options;
    QString error;
    if (!parseOptions(arguments, &options, &error)) {
        std::fprintf(stderr, "terminal-stress-tui: %s\n\n%s", qPrintable(error),
                     usageText().toUtf8().constData());
        return 2;
    }
    if (options.showHelp) {
        std::fputs(usageText().toUtf8().constData(), stdout);
        return 0;
    }
    if (options.listScenarios) {
        for (const QString &name : scenarioNames()) std::printf("%s\n", qPrintable(name));
        return 0;
    }

    prepareConsole();
    Driver driver(options);
    driver.start();
    return app.exec();
}
