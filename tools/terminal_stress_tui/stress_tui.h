#pragma once

#include <QByteArray>
#include <QRandomGenerator>
#include <QString>
#include <QStringList>
#include <QVector>

#include <memory>

namespace spantui::stresstui {

struct TerminalSize {
    int columns = 80;
    int rows = 24;
    friend bool operator==(TerminalSize a, TerminalSize b) {
        return a.columns == b.columns && a.rows == b.rows;
    }
    friend bool operator!=(TerminalSize a, TerminalSize b) { return !(a == b); }
};

enum class Scenario {
    OmpTranscript,
    FullscreenChurn,
    LogFlood,
    StallBurst,
    BrokenProtocol,
    Chaos,    // switches among the scenarios above
    Protocol, // well-behaved VT protocol coverage pages; not part of chaos
};

struct Options {
    Scenario scenario = Scenario::Chaos;
    quint64 seed = 0x5eed;
    int durationMs = 0;          // 0 runs until Ctrl+C
    int fps = 60;                // target frame rate of frame-driven scenarios
    int jankPercent = 30;        // chance that one frame is split and stalled mid-write
    int stallMaxMs = 400;        // upper bound of one mid-frame stall
    int fragmentMaxBytes = 4096; // upper bound of one write when a frame is split
    int transcriptLimitKiB = 4096;
    int chaosMinMs = 3000;
    int chaosMaxMs = 10000;
    int protocolStage = -1;      // protocol scenario: pin one stage, -1 cycles all
    bool synchronized = false;   // wrap frames in DEC 2026 (inbox ConPTY drops it)
    bool showHelp = false;
    bool listScenarios = false;
};

bool parseOptions(const QStringList &arguments, Options *options, QString *error);
QString usageText();
QStringList scenarioNames();
QString scenarioName(Scenario scenario);

// One WriteFile call; the driver blocks for stallAfterMs before the next one.
struct WriteStep {
    QByteArray bytes;
    int stallAfterMs = 0;
};
using WritePlan = QVector<WriteStep>;

QByteArray concatenated(const WritePlan &plan);

// Display width of one code point as the terminal grid counts it.
int codepointWidth(char32_t codepoint);
// Visible width of styled text; CSI sequences take no cells.
int visibleWidth(const QString &styled);
// Truncates styled text to `columns` cells and, when `pad`, fills it with
// spaces to exactly that width. A wide glyph that would straddle the edge
// becomes a space. Styled input is closed with SGR 0.
QString fitToWidth(const QString &styled, int columns, bool pad = true);
// Greedy wrap of plain text into lines of at most `columns` cells.
QStringList wrapPlain(const QString &plain, int columns);

// Same 7-cell stamp on every row of one frame: rows with different stamps on
// screen at the same time are a torn frame.
QString frameStamp(quint64 frame);

// Splits writes into random fragments and inserts one mid-frame stall,
// modelling a TUI that blocks on work between partial writes.
class Jank {
public:
    Jank(const Options &options, QRandomGenerator *random);
    WritePlan apply(const WritePlan &plan);

private:
    Options m_options;
    QRandomGenerator *m_random;
};

class ScenarioEngine {
public:
    virtual ~ScenarioEngine() = default;
    // Bytes that set up the scenario's screen state.
    virtual QByteArray enter(TerminalSize size) = 0;
    virtual WritePlan tick(TerminalSize size) = 0;
    // Bytes that undo every mode the scenario may have left set.
    virtual QByteArray leave() = 0;
    virtual int nextDelayMs() const = 0;
    // Whether the driver may fragment this scenario's writes. Scenarios that
    // already choose their own write boundaries opt out.
    virtual bool acceptsJank() const { return true; }
};

std::unique_ptr<ScenarioEngine> createScenario(Scenario scenario, const Options &options,
                                               QRandomGenerator *random);

// Resets every mode any scenario uses; written on exit and on chaos switches.
QByteArray resetSequence();

// Inline (main-screen) chat renderer modelled on pi-tui: rewrites from the
// first changed line, and clears screen plus scrollback (ED2+ED3) and reprints
// the whole transcript when that line has scrolled above the viewport or the
// width changed. Exposed for tests.
class InlineRenderer {
public:
    explicit InlineRenderer(bool synchronized) : m_synchronized(synchronized) {}
    QByteArray render(const QStringList &lines, TerminalSize size);
    bool lastRenderWasFullReprint() const { return m_lastFull; }
    void reset();

private:
    bool m_synchronized;
    QStringList m_previous;
    int m_previousColumns = -1;
    bool m_lastFull = false;
};

} // namespace spantui::stresstui
