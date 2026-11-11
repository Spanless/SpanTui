#include "stress_tui.h"
#include "protocol_stages.h"

#include <QElapsedTimer>

#include <algorithm>
#include <array>
#include <utility>

namespace spantui::stresstui {

namespace {

struct ScenarioEntry {
    Scenario scenario;
    const char *name;
    const char *summary;
};

constexpr std::array<ScenarioEntry, 7> kScenarios{{
    {Scenario::OmpTranscript, "omp-transcript",
     "inline chat transcript: diff rewrites, spinner, ED2+ED3 whole-transcript reprints"},
    {Scenario::FullscreenChurn, "fullscreen-churn",
     "alternate-screen dashboard redrawn every frame, every row stamped"},
    {Scenario::LogFlood, "log-flood", "main-screen log flood with wrapping lines and CR progress bars"},
    {Scenario::StallBurst, "stall-burst", "spinner, a long freeze, then one huge write"},
    {Scenario::BrokenProtocol, "broken-protocol",
     "lost/duplicate 2026, split escapes and UTF-8, out-of-range parameters"},
    {Scenario::Chaos, "chaos", "switches between the scenarios above at random intervals"},
    {Scenario::Protocol, "protocol",
     "well-behaved VT protocol coverage pages, one protocol family per stage"},
}};

constexpr int kProtocolStageMs = 4000;

constexpr char kEsc = '\x1b';
const QString kSgrReset = QStringLiteral("\x1b[0m");

void appendCodepoint(QString &text, char32_t codepoint) {
    if (QChar::requiresSurrogates(codepoint)) {
        text += QChar(QChar::highSurrogate(codepoint));
        text += QChar(QChar::lowSurrogate(codepoint));
    } else {
        text += QChar(char16_t(codepoint));
    }
}

struct Token {
    bool escape = false;
    QString text;
    int width = 0;
};

// Splits styled text into zero-width escape sequences and single glyphs.
QVector<Token> tokenize(const QString &styled) {
    QVector<Token> tokens;
    const QList<uint> codepoints = styled.toUcs4();
    qsizetype index = 0;
    while (index < codepoints.size()) {
        const char32_t codepoint = codepoints.at(index);
        if (codepoint == char32_t(kEsc) && index + 1 < codepoints.size()) {
            Token token{true, {}, 0};
            const char32_t introducer = codepoints.at(index + 1);
            appendCodepoint(token.text, codepoint);
            appendCodepoint(token.text, introducer);
            index += 2;
            if (introducer == '[') {
                while (index < codepoints.size()) {
                    const char32_t byte = codepoints.at(index++);
                    appendCodepoint(token.text, byte);
                    if (byte >= 0x40 && byte <= 0x7e) break;
                }
            } else if (introducer == ']') {
                while (index < codepoints.size()) {
                    const char32_t byte = codepoints.at(index++);
                    appendCodepoint(token.text, byte);
                    if (byte == 0x07) break;
                    if (byte == char32_t(kEsc) && index < codepoints.size()
                        && codepoints.at(index) == '\\') {
                        appendCodepoint(token.text, codepoints.at(index++));
                        break;
                    }
                }
            }
            tokens.push_back(std::move(token));
            continue;
        }
        Token token{false, {}, codepointWidth(codepoint)};
        appendCodepoint(token.text, codepoint);
        tokens.push_back(std::move(token));
        ++index;
    }
    return tokens;
}

QString csi(int value, char final) {
    return QStringLiteral("\x1b[%1%2").arg(value).arg(QLatin1Char(final));
}

QByteArray wrapSynchronized(QByteArray body, bool synchronized) {
    if (!synchronized || body.isEmpty()) return body;
    return QByteArrayLiteral("\x1b[?2026h") + body + QByteArrayLiteral("\x1b[?2026l");
}

int frameIntervalMs(const Options &options) {
    return std::max(1, 1000 / std::clamp(options.fps, 1, 360));
}

int boundedInt(QRandomGenerator *random, int lowest, int highest) {
    if (highest <= lowest) return lowest;
    return lowest + int(random->bounded(quint32(highest - lowest + 1)));
}

bool chance(QRandomGenerator *random, int percent) {
    return percent > 0 && int(random->bounded(100u)) < percent;
}

template <typename T, std::size_t N>
const T &pick(QRandomGenerator *random, const std::array<T, N> &items) {
    return items[random->bounded(quint32(N))];
}

const std::array<const char *, 40> kWords{{
    "the",
    "frame",
    "snapshot",
    "parser",
    "queue",
    "ConPTY",
    "resize",
    "cursor",
    "damage",
    "render",
    "transcript",
    "token",
    "stream",
    "worker",
    "epoch",
    "boundary",
    "scrollback",
    "终端",
    "渲染",
    "撕裂",
    "同步",
    "快照",
    "解析器",
    "回滚",
    "重绘",
    "压力测试",
    "输出队列",
    "光标",
    "代次",
    "边界",
    "`publishSnapshot()`",
    "`drainOutput`",
    "TerminalView",
    "QSG",
    "libghostty",
    "ED3",
    "DECSET",
    "2026",
    "0x1b",
    "ok",
}};

const std::array<const char *, 8> kLevels{{"TRACE", "DEBUG", "INFO ", "INFO ", "INFO ", "WARN ",
                                           "ERROR", "INFO "}};
const std::array<const char *, 8> kModules{{"core::worker", "pty::reader", "render::sg",
                                            "net::fetch", "db::pool", "终端::会话", "ui::layout",
                                            "build::ninja"}};
const std::array<const char *, 10> kSpinner{{"⠋", "⠙", "⠹", "⠸", "⠼", "⠴", "⠦", "⠧", "⠇", "⠏"}};
const std::array<const char *, 6> kTools{{"bash", "read", "edit", "grep", "write", "web_fetch"}};
const std::array<const char *, 6> kBandColors{{"28;32;44", "44;28;32", "28;44;32", "44;40;24",
                                               "36;28;48", "24;40;44"}};

QString words(QRandomGenerator *random, int count) {
    QString text;
    for (int index = 0; index < count; ++index) {
        if (!text.isEmpty()) text += QLatin1Char(' ');
        text += QString::fromUtf8(pick(random, kWords));
    }
    return text;
}

QString logLine(QRandomGenerator *random, quint64 serial) {
    const quint64 millis = serial * 7 % 86'400'000;
    return QStringLiteral("[%1:%2:%3.%4] %5 %6  %7")
        .arg(millis / 3'600'000 % 24, 2, 10, QLatin1Char('0'))
        .arg(millis / 60'000 % 60, 2, 10, QLatin1Char('0'))
        .arg(millis / 1000 % 60, 2, 10, QLatin1Char('0'))
        .arg(millis % 1000, 3, 10, QLatin1Char('0'))
        .arg(QString::fromUtf8(pick(random, kLevels)))
        .arg(QString::fromUtf8(pick(random, kModules)))
        .arg(words(random, boundedInt(random, 3, 14)));
}

QString levelColored(const QString &line) {
    if (line.contains(QStringLiteral("ERROR"))) return QStringLiteral("\x1b[1;31m") + line + kSgrReset;
    if (line.contains(QStringLiteral("WARN"))) return QStringLiteral("\x1b[33m") + line + kSgrReset;
    if (line.contains(QStringLiteral("TRACE")) || line.contains(QStringLiteral("DEBUG"))) {
        return QStringLiteral("\x1b[2m") + line + kSgrReset;
    }
    return line;
}

QString blockBar(double fraction, int cells) {
    static const std::array<const char *, 8> partial{{"▏", "▎", "▍", "▌", "▋", "▊", "▉", "█"}};
    QString bar;
    const double filled = std::clamp(fraction, 0.0, 1.0) * cells;
    const int full = int(filled);
    for (int index = 0; index < full; ++index) bar += QStringLiteral("█");
    if (full < cells) {
        const int eighth = int((filled - full) * 8);
        bar += eighth > 0 ? QString::fromUtf8(partial[eighth - 1]) : QStringLiteral(" ");
        for (int index = full + 1; index < cells; ++index) bar += QLatin1Char(' ');
    }
    return bar;
}

// Dashboard body shared by the alternate-screen scenarios. Every row ends in
// the same frame stamp over the same band colour, so a torn frame shows two
// stamps and two bands at once.
QByteArray stampedDashboard(TerminalSize size, quint64 frame, QRandomGenerator *random,
                            const QString &title) {
    const QString stamp = frameStamp(frame);
    const QString band = QStringLiteral("\x1b[48;2;%1m").arg(QString::fromUtf8(kBandColors[frame % kBandColors.size()]));
    const int bodyColumns = std::max(2, size.columns - 9); // " │" + 7-cell stamp
    QString out;
    for (int row = 0; row < size.rows; ++row) {
        QString body;
        if (row == 0) {
            body = QStringLiteral("\x1b[1m%1 · 帧 %2 · %3x%4\x1b[22m")
                       .arg(title).arg(frame).arg(size.columns).arg(size.rows);
        } else if (row % 7 == 1) {
            body = QStringLiteral("├") + QString(bodyColumns - 2, QChar(0x2500)) + QStringLiteral("┤");
        } else if (row % 3 == 0) {
            const double load = random->generateDouble();
            body = QStringLiteral("CPU%1 [\x1b[38;2;%2;%3;80m%4\x1b[39m] %5%")
                       .arg(row % 64, 2, 10, QLatin1Char('0'))
                       .arg(int(80 + load * 175)).arg(int(220 - load * 160))
                       .arg(blockBar(load, std::max(4, bodyColumns / 2)))
                       .arg(load * 100, 5, 'f', 1);
        } else if (row % 3 == 1) {
            QString braille;
            for (int cell = 0; cell < bodyColumns / 3; ++cell) {
                appendCodepoint(braille, char32_t(0x2800 + random->bounded(256u)));
            }
            body = QStringLiteral("进程 %1 │ %2").arg(random->bounded(99999u), 5).arg(braille);
        } else {
            body = QStringLiteral("\x1b[38;5;%1m%2\x1b[39m")
                       .arg(random->bounded(256u))
                       .arg(words(random, std::max(1, bodyColumns / 6)));
        }
        out += QStringLiteral("\x1b[%1;1H").arg(row + 1) + band
            + fitToWidth(band + body, bodyColumns) + band + QStringLiteral(" │") + stamp
            + kSgrReset;
    }
    return out.toUtf8();
}

class OmpTranscriptScenario final : public ScenarioEngine {
public:
    OmpTranscriptScenario(const Options &options, QRandomGenerator *random)
        : m_options(options), m_random(random), m_renderer(options.synchronized) {}

    QByteArray enter(TerminalSize) override {
        m_renderer.reset();
        return QByteArrayLiteral("\x1b[?25l");
    }

    WritePlan tick(TerminalSize size) override {
        ++m_frame;
        advance();
        const QByteArray bytes = m_renderer.render(lines(size), size);
        if (bytes.isEmpty()) return {};
        return {{bytes, 0}};
    }

    QByteArray leave() override { return QByteArrayLiteral("\r\n\x1b[?25h"); }

    int nextDelayMs() const override { return frameIntervalMs(m_options); }

private:
    enum class Kind { User, Assistant, Tool };
    struct Block {
        Kind kind = Kind::Assistant;
        QString title;
        QString text;
        QStringList output;
        bool running = true;
        bool collapsed = false;
        quint64 startedFrame = 0;
    };

    void advance() {
        if (m_blocks.isEmpty() || !m_blocks.back().running) startBlock();
        Block &current = m_blocks.back();
        if (current.kind == Kind::Assistant) {
            current.text += QLatin1Char(' ') + words(m_random, boundedInt(m_random, 1, 30));
            m_tokens += quint64(boundedInt(m_random, 1, 30));
            if (chance(m_random, 4)) current.running = false;
        } else if (current.kind == Kind::Tool) {
            for (int line = boundedInt(m_random, 0, 3); line > 0; --line) {
                current.output.push_back(logLine(m_random, ++m_logSerial));
            }
            // The header status flips when the tool finishes; with long output
            // it is above the viewport by then and forces a whole reprint.
            if (chance(m_random, 5)) current.running = false;
        }
        // An older tool block toggles collapse: a change far above the viewport.
        if (m_blocks.size() > 3 && chance(m_random, 1)) {
            Block &older = m_blocks[qsizetype(m_random->bounded(quint32(m_blocks.size() - 1)))];
            if (older.kind == Kind::Tool) older.collapsed = !older.collapsed;
        }
        while (m_blocks.size() > 1 && transcriptBytes() > qsizetype(m_options.transcriptLimitKiB) * 1024) {
            m_blocks.removeFirst();
        }
    }

    void startBlock() {
        Block block;
        block.startedFrame = m_frame;
        const int roll = int(m_random->bounded(100u));
        if (roll < 15) {
            block.kind = Kind::User;
            block.text = words(m_random, boundedInt(m_random, 4, 40));
            block.running = false;
        } else if (roll < 70) {
            block.kind = Kind::Assistant;
        } else {
            block.kind = Kind::Tool;
            block.title = QString::fromUtf8(pick(m_random, kTools));
            block.text = words(m_random, boundedInt(m_random, 2, 6));
        }
        m_blocks.push_back(std::move(block));
    }

    qsizetype transcriptBytes() const {
        qsizetype total = 0;
        for (const Block &block : m_blocks) {
            total += block.text.size() * 2;
            for (const QString &line : block.output) total += line.size() * 2;
        }
        return total;
    }

    QStringList lines(TerminalSize size) const {
        const int columns = std::max(10, size.columns);
        QStringList out;
        for (const Block &block : m_blocks) {
            if (!out.isEmpty()) out.push_back(QString());
            switch (block.kind) {
            case Kind::User:
                for (const QString &line : wrapPlain(block.text, columns - 2)) {
                    out.push_back(QStringLiteral("\x1b[1;36m› \x1b[22m") + line + kSgrReset);
                }
                break;
            case Kind::Assistant:
                for (const QString &line : wrapPlain(block.text, columns)) out.push_back(line);
                break;
            case Kind::Tool: {
                // Static while running: an animated header that scrolled off
                // would force a whole reprint on every frame.
                const QString status = block.running ? QStringLiteral("\x1b[33m… running\x1b[39m")
                                                     : QStringLiteral("\x1b[32m✓ done\x1b[39m");
                out.push_back(fitToWidth(QStringLiteral("\x1b[1;33m● %1\x1b[22;39m(%2)  %3")
                                             .arg(block.title, block.text, status),
                                         columns, false));
                QStringList body = block.output;
                if (block.collapsed && body.size() > 5) {
                    const qsizetype hidden = body.size() - 5;
                    body = body.mid(hidden);
                    body.prepend(QStringLiteral("… %1 more lines").arg(hidden));
                }
                for (const QString &line : body) {
                    out.push_back(fitToWidth(QStringLiteral("\x1b[2m  │ ") + line, columns, false));
                }
                break;
            }
            }
        }
        // Bottom chrome rewritten every frame. Both stamped lines carry the
        // same frame number; a mismatch on screen is a torn diff render.
        const QString stamp = frameStamp(m_frame);
        const int inner = columns - 2;
        out.push_back(QString());
        out.push_back(QStringLiteral("╭") + QString(inner, QChar(0x2500)) + QStringLiteral("╮"));
        const QString draft = QStringLiteral("> ") + QString(int(m_frame % 23), QLatin1Char('.'));
        out.push_back(QStringLiteral("│") + fitToWidth(draft, inner) + QStringLiteral("│"));
        out.push_back(fitToWidth(QStringLiteral("╰── ") + stamp + QLatin1Char(' ')
                                     + QString(columns, QChar(0x2500)),
                                 columns - 1, false)
                      + QStringLiteral("╯"));
        const bool working = !m_blocks.isEmpty() && m_blocks.back().running;
        out.push_back(fitToWidth(
            QStringLiteral("\x1b[2m%1 %2 · %3s · %4 tokens · %5 · %6\x1b[22m")
                .arg(QString::fromUtf8(kSpinner[m_frame % kSpinner.size()]),
                     working ? QStringLiteral("Working…") : QStringLiteral("Idle"))
                .arg(double(m_frame) * frameIntervalMs(m_options) / 1000.0, 0, 'f', 1)
                .arg(m_tokens)
                .arg(stamp)
                .arg(m_renderer.lastRenderWasFullReprint() ? QStringLiteral("reprint")
                                                           : QStringLiteral("diff")),
            columns, false));
        return out;
    }

    Options m_options;
    QRandomGenerator *m_random;
    InlineRenderer m_renderer;
    QVector<Block> m_blocks;
    quint64 m_frame = 0;
    quint64 m_tokens = 0;
    quint64 m_logSerial = 0;
};

class FullscreenChurnScenario final : public ScenarioEngine {
public:
    FullscreenChurnScenario(const Options &options, QRandomGenerator *random)
        : m_options(options), m_random(random) {}

    QByteArray enter(TerminalSize) override {
        return QByteArrayLiteral("\x1b[?1049h\x1b[?25l\x1b[2J");
    }

    WritePlan tick(TerminalSize size) override {
        ++m_frame;
        return {{wrapSynchronized(stampedDashboard(size, m_frame, m_random,
                                                   QStringLiteral("fullscreen-churn")),
                                  m_options.synchronized),
                 0}};
    }

    QByteArray leave() override { return QByteArrayLiteral("\x1b[0m\x1b[?1049l\x1b[?25h"); }
    int nextDelayMs() const override { return frameIntervalMs(m_options); }

private:
    Options m_options;
    QRandomGenerator *m_random;
    quint64 m_frame = 0;
};

class LogFloodScenario final : public ScenarioEngine {
public:
    LogFloodScenario(const Options &options, QRandomGenerator *random)
        : m_options(options), m_random(random) {}

    QByteArray enter(TerminalSize) override { return QByteArrayLiteral("\x1b[?25l"); }

    WritePlan tick(TerminalSize size) override {
        QString out;
        const int count = boundedInt(m_random, 50, 600);
        for (int index = 0; index < count; ++index) {
            QString line = logLine(m_random, ++m_serial);
            if (chance(m_random, 8)) {
                // Wraps across several rows.
                while (visibleWidth(line) < size.columns * boundedInt(m_random, 2, 4)) {
                    line += QLatin1Char(' ') + words(m_random, 8);
                }
            }
            out += levelColored(line) + QStringLiteral("\r\n");
            if (chance(m_random, 2)) {
                // A CR-overwritten progress bar inside the same write.
                for (int percent = 0; percent <= 100; percent += 10) {
                    out += QStringLiteral("\r\x1b[2K[%1] %2%")
                               .arg(blockBar(percent / 100.0, 30))
                               .arg(percent, 3);
                }
                out += QStringLiteral("\r\n");
            }
        }
        return {{wrapSynchronized(out.toUtf8(), m_options.synchronized), 0}};
    }

    QByteArray leave() override { return QByteArrayLiteral("\x1b[0m\x1b[?25h"); }
    int nextDelayMs() const override {
        return boundedInt(m_random, 0, frameIntervalMs(m_options));
    }

private:
    Options m_options;
    QRandomGenerator *m_random;
    quint64 m_serial = 0;
};

class StallBurstScenario final : public ScenarioEngine {
public:
    StallBurstScenario(const Options &options, QRandomGenerator *random)
        : m_options(options), m_random(random) {}

    QByteArray enter(TerminalSize) override {
        m_spinnerTicks = boundedInt(m_random, 10, 60);
        return QByteArrayLiteral("\x1b[?25l");
    }

    WritePlan tick(TerminalSize size) override {
        ++m_frame;
        if (m_spinnerTicks > 0) {
            --m_spinnerTicks;
            return {{QStringLiteral("\r\x1b[2K%1 waiting for model… %2")
                         .arg(QString::fromUtf8(kSpinner[m_frame % kSpinner.size()]), frameStamp(m_frame))
                         .toUtf8(),
                     0}};
        }
        m_spinnerTicks = boundedInt(m_random, 10, 60);
        // The process blocks with nothing on the wire, then writes everything
        // it produced meanwhile in one call.
        const int freezeMs = boundedInt(m_random, 300, std::max(300, m_options.stallMaxMs * 4));
        const qsizetype target = qsizetype(boundedInt(m_random, 256, 1024)) * 1024;
        QString burst = QStringLiteral("\r\x1b[2K");
        while (burst.size() < target / 2) {
            burst += levelColored(logLine(m_random, ++m_serial)) + QStringLiteral("\r\n");
        }
        burst += QStringLiteral("\x1b[1m── burst %1 · %2x%3 ──\x1b[22m\r\n")
                     .arg(frameStamp(m_frame)).arg(size.columns).arg(size.rows);
        return {{QByteArray(), freezeMs}, {burst.toUtf8(), 0}};
    }

    QByteArray leave() override { return QByteArrayLiteral("\r\n\x1b[0m\x1b[?25h"); }
    int nextDelayMs() const override { return 33; }
    bool acceptsJank() const override { return false; }

private:
    Options m_options;
    QRandomGenerator *m_random;
    quint64 m_frame = 0;
    quint64 m_serial = 0;
    int m_spinnerTicks = 0;
};

class BrokenProtocolScenario final : public ScenarioEngine {
public:
    BrokenProtocolScenario(const Options &options, QRandomGenerator *random)
        : m_options(options), m_random(random) {}

    QByteArray enter(TerminalSize) override {
        return QByteArrayLiteral("\x1b[?1049h\x1b[?25l\x1b[2J");
    }

    WritePlan tick(TerminalSize size) override {
        ++m_frame;
        const QByteArray frame = stampedDashboard(
            size, m_frame, m_random, QStringLiteral("broken-protocol #%1").arg(m_case));
        WritePlan plan;
        switch (m_case) {
        case 0: // 2026 set, matching reset lost.
            plan = {{QByteArrayLiteral("\x1b[?2026h") + frame, 0}};
            break;
        case 1: // Repeated set and a stray reset.
            plan = {{QByteArrayLiteral("\x1b[?2026h\x1b[?2026h") + frame
                         + QByteArrayLiteral("\x1b[?2026l\x1b[?2026l"),
                     0}};
            break;
        case 2: // Every escape sequence split right after ESC and inside CSI.
            plan = splitAt(frame, [](const QByteArray &bytes, qsizetype index) {
                return bytes.at(index) == kEsc
                    || (bytes.at(index - 1) == kEsc && bytes.at(index) == '[')
                    || bytes.at(index - 1) == ';';
            });
            break;
        case 3: // UTF-8 split between lead and continuation bytes.
            plan = splitAt(frame, [this](const QByteArray &bytes, qsizetype index) {
                return (uchar(bytes.at(index)) & 0xc0) == 0x80 && m_random->bounded(4u) == 0;
            }, 1);
            break;
        case 4: { // Out-of-range and oversized parameters before a normal frame.
            QByteArray bad = QByteArrayLiteral(
                "\x1b[9999;9999H\x1b[0;0r\x1b[38;2;999;-1;5m\x1b[?9999h\x1b[99999@\x1b[r");
            bad += QByteArrayLiteral("\x1b[");
            for (int index = 0; index < 300; ++index) bad += QByteArrayLiteral("1;");
            bad += QByteArrayLiteral("m\x1b[0m");
            plan = {{bad + frame, 0}};
            break;
        }
        case 5: { // Unterminated OSC title swallowing a frame, closed later.
            const QByteArray title = QByteArrayLiteral("\x1b]0;stress ") + QByteArray(2000, 'x');
            plan = {{title, boundedInt(m_random, 50, 300)}, {frame, 0},
                    {QByteArrayLiteral("\x1b\\"), 0}, {frame, 0}};
            break;
        }
        case 6: { // A slow producer stalls inside a 2026 transaction.
            const qsizetype half = frame.size() / 2;
            plan = {{QByteArrayLiteral("\x1b[?2026h") + frame.left(half),
                     boundedInt(m_random, 100, 700)},
                    {frame.mid(half) + QByteArrayLiteral("\x1b[?2026l"), 0}};
            break;
        }
        }
        m_case = (m_case + 1) % 7;
        return plan;
    }

    QByteArray leave() override {
        return QByteArrayLiteral("\x18\x1b\\\x1b[?2026l\x1b[0m\x1b[r\x1b[?1049l\x1b[?25h");
    }
    int nextDelayMs() const override { return std::max(frameIntervalMs(m_options), 50); }
    bool acceptsJank() const override { return false; }

private:
    template <typename Predicate>
    WritePlan splitAt(const QByteArray &bytes, Predicate split, int stallMs = 0) {
        WritePlan plan;
        qsizetype start = 0;
        for (qsizetype index = 1; index < bytes.size(); ++index) {
            if (split(bytes, index)) {
                plan.push_back({bytes.mid(start, index - start), stallMs});
                start = index;
            }
        }
        plan.push_back({bytes.mid(start), 0});
        return plan;
    }

    Options m_options;
    QRandomGenerator *m_random;
    quint64 m_frame = 0;
    int m_case = 0;
};

// Cycles the protocol coverage pages (or stays on --protocol-stage). Each
// frame is one complete write: these pages check rendering, not tearing.
class ProtocolScenario final : public ScenarioEngine {
public:
    explicit ProtocolScenario(const Options &options) : m_options(options) {}

    QByteArray enter(TerminalSize) override {
        return QByteArrayLiteral("\x1b[?1049h\x1b[?25l\x1b[2J\x1b[H");
    }

    WritePlan tick(TerminalSize size) override {
        const quint64 framesPerStage =
            quint64(std::max(1, kProtocolStageMs / frameIntervalMs(m_options)));
        constexpr quint64 stageCount = quint64(protocol::Stage::Count);
        const quint64 stage = m_options.protocolStage >= 0
            ? quint64(m_options.protocolStage)
            : m_frame / framesPerStage % stageCount;
        const quint64 frameInStage = m_options.protocolStage >= 0
            ? m_frame : m_frame % framesPerStage;
        const bool firstFrame = frameInStage == 0 || size != m_size;
        m_size = size;
        ++m_frame;
        return {{QByteArray::fromStdString(protocol::renderFrame(
                     {protocol::Stage(stage), size, frameInStage, firstFrame,
                      m_options.synchronized})),
                 0}};
    }

    QByteArray leave() override {
        return QByteArray::fromStdString(protocol::resetModes())
            + QByteArrayLiteral("\x1b[?1049l");
    }
    int nextDelayMs() const override { return frameIntervalMs(m_options); }
    bool acceptsJank() const override { return false; }

private:
    Options m_options;
    TerminalSize m_size;
    quint64 m_frame = 0;
};

class ChaosScenario final : public ScenarioEngine {
public:
    ChaosScenario(const Options &options, QRandomGenerator *random)
        : m_options(options), m_random(random) {}

    QByteArray enter(TerminalSize size) override {
        switchScenario();
        return m_current->enter(size);
    }

    WritePlan tick(TerminalSize size) override {
        WritePlan plan;
        if (m_clock.elapsed() >= m_switchAfterMs) {
            QByteArray transition = m_current->leave() + resetSequence();
            switchScenario();
            transition += m_current->enter(size);
            plan.push_back({transition, 0});
        }
        plan += m_current->tick(size);
        return plan;
    }

    QByteArray leave() override { return m_current ? m_current->leave() : QByteArray(); }
    int nextDelayMs() const override { return m_current->nextDelayMs(); }
    bool acceptsJank() const override { return m_current->acceptsJank(); }

private:
    void switchScenario() {
        Scenario next = Scenario(m_random->bounded(quint32(Scenario::Chaos)));
        m_current = createScenario(next, m_options, m_random);
        m_switchAfterMs = boundedInt(m_random, m_options.chaosMinMs,
                                     std::max(m_options.chaosMinMs, m_options.chaosMaxMs));
        m_clock.start();
    }

    Options m_options;
    QRandomGenerator *m_random;
    std::unique_ptr<ScenarioEngine> m_current;
    QElapsedTimer m_clock;
    qint64 m_switchAfterMs = 0;
};

bool parseInt(const QString &text, int lowest, int highest, int *value) {
    bool ok = false;
    const int parsed = text.toInt(&ok);
    if (!ok || parsed < lowest || parsed > highest) return false;
    *value = parsed;
    return true;
}

} // namespace

QStringList scenarioNames() {
    QStringList names;
    for (const ScenarioEntry &entry : kScenarios) names.push_back(QString::fromLatin1(entry.name));
    return names;
}

QString scenarioName(Scenario scenario) {
    for (const ScenarioEntry &entry : kScenarios) {
        if (entry.scenario == scenario) return QString::fromLatin1(entry.name);
    }
    return {};
}

QString usageText() {
    QString text = QStringLiteral(
        "Usage: terminal-stress-tui [options]\n"
        "\n"
        "A deliberately badly behaved TUI for stressing a terminal host.\n"
        "Rows of one frame share a stamp (F000123); two stamps on screen at\n"
        "once is a torn frame. Same --seed gives the same byte stream.\n"
        "\n"
        "  --scenario <name>        scenario to run (default chaos)\n"
        "  --seed <n>               random seed (default 24301)\n"
        "  --duration-ms <n>        stop after n ms (default 0 = until Ctrl+C)\n"
        "  --fps <n>                frame rate of frame-driven scenarios, 1..360 (60)\n"
        "  --jank <percent>         chance a frame is split and stalled mid-write (30)\n"
        "  --stall-max-ms <n>       longest mid-frame stall, 0..10000 (400)\n"
        "  --fragment-max <bytes>   longest write of a split frame (4096)\n"
        "  --transcript-limit-kib <n>  omp-transcript size before old blocks drop (4096)\n"
        "  --chaos-min-ms <n> / --chaos-max-ms <n>  chaos switch interval (3000/10000)\n"
        "  --protocol-stage <name>  protocol scenario: stay on one stage (default cycles all)\n"
        "  --sync                   wrap frames in DEC 2026 synchronized updates\n"
        "  --list                   list scenarios\n"
        "  -h, --help               show this help\n"
        "\n"
        "Scenarios:\n");
    for (const ScenarioEntry &entry : kScenarios) {
        text += QStringLiteral("  %1%2\n")
                    .arg(QString::fromLatin1(entry.name), -18)
                    .arg(QString::fromLatin1(entry.summary));
    }
    text += QStringLiteral("\nProtocol stages (--protocol-stage):\n");
    for (const std::string_view stage : protocol::stageNames())
        text += QStringLiteral("  %1\n").arg(QLatin1String(stage.data(), qsizetype(stage.size())));
    return text;
}

bool parseOptions(const QStringList &arguments, Options *options, QString *error) {
    Options parsed;
    for (qsizetype index = 0; index < arguments.size(); ++index) {
        QString name = arguments.at(index);
        QString value;
        bool hasInlineValue = false;
        if (name.startsWith(QStringLiteral("--")) && name.contains(QLatin1Char('='))) {
            value = name.section(QLatin1Char('='), 1);
            name = name.section(QLatin1Char('='), 0, 0);
            hasInlineValue = true;
        }
        auto takeValue = [&](QString *out) {
            if (hasInlineValue) {
                *out = value;
                return true;
            }
            if (index + 1 >= arguments.size()) {
                *error = QStringLiteral("%1 needs a value").arg(name);
                return false;
            }
            *out = arguments.at(++index);
            return true;
        };
        auto takeInt = [&](int lowest, int highest, int *out) {
            QString text;
            if (!takeValue(&text)) return false;
            if (!parseInt(text, lowest, highest, out)) {
                *error = QStringLiteral("%1 expects an integer in %2..%3, got '%4'")
                             .arg(name).arg(lowest).arg(highest).arg(text);
                return false;
            }
            return true;
        };

        if (name == QStringLiteral("-h") || name == QStringLiteral("--help")) {
            parsed.showHelp = true;
        } else if (name == QStringLiteral("--list")) {
            parsed.listScenarios = true;
        } else if (name == QStringLiteral("--sync")) {
            parsed.synchronized = true;
        } else if (name == QStringLiteral("--scenario")) {
            QString text;
            if (!takeValue(&text)) return false;
            const auto found = std::find_if(kScenarios.begin(), kScenarios.end(),
                [&](const ScenarioEntry &entry) { return text == QLatin1String(entry.name); });
            if (found == kScenarios.end()) {
                *error = QStringLiteral("unknown scenario '%1'").arg(text);
                return false;
            }
            parsed.scenario = found->scenario;
        } else if (name == QStringLiteral("--seed")) {
            QString text;
            if (!takeValue(&text)) return false;
            bool ok = false;
            parsed.seed = text.toULongLong(&ok, 0);
            if (!ok) {
                *error = QStringLiteral("--seed expects an unsigned integer, got '%1'").arg(text);
                return false;
            }
        } else if (name == QStringLiteral("--duration-ms")) {
            if (!takeInt(0, 86'400'000, &parsed.durationMs)) return false;
        } else if (name == QStringLiteral("--fps")) {
            if (!takeInt(1, 360, &parsed.fps)) return false;
        } else if (name == QStringLiteral("--jank")) {
            if (!takeInt(0, 100, &parsed.jankPercent)) return false;
        } else if (name == QStringLiteral("--stall-max-ms")) {
            if (!takeInt(0, 10'000, &parsed.stallMaxMs)) return false;
        } else if (name == QStringLiteral("--fragment-max")) {
            if (!takeInt(1, 1 << 20, &parsed.fragmentMaxBytes)) return false;
        } else if (name == QStringLiteral("--transcript-limit-kib")) {
            if (!takeInt(16, 65'536, &parsed.transcriptLimitKiB)) return false;
        } else if (name == QStringLiteral("--chaos-min-ms")) {
            if (!takeInt(100, 600'000, &parsed.chaosMinMs)) return false;
        } else if (name == QStringLiteral("--chaos-max-ms")) {
            if (!takeInt(100, 600'000, &parsed.chaosMaxMs)) return false;
        } else if (name == QStringLiteral("--protocol-stage")) {
            QString text;
            if (!takeValue(&text)) return false;
            const auto stages = protocol::stageNames();
            const auto found = std::find(stages.begin(), stages.end(), text.toStdString());
            if (found == stages.end()) {
                *error = QStringLiteral("unknown protocol stage '%1'").arg(text);
                return false;
            }
            parsed.protocolStage = int(found - stages.begin());
        } else {
            *error = QStringLiteral("unknown option '%1'").arg(name);
            return false;
        }
    }
    if (parsed.chaosMaxMs < parsed.chaosMinMs) {
        *error = QStringLiteral("--chaos-max-ms must not be below --chaos-min-ms");
        return false;
    }
    *options = parsed;
    return true;
}

QByteArray concatenated(const WritePlan &plan) {
    QByteArray bytes;
    for (const WriteStep &step : plan) bytes += step.bytes;
    return bytes;
}

int codepointWidth(char32_t c) {
    if (c < 0x20 || (c >= 0x7f && c < 0xa0)) return 0;
    if ((c >= 0x0300 && c <= 0x036f) || (c >= 0x20d0 && c <= 0x20ff) || c == 0x200b
        || c == 0x200d || (c >= 0xfe00 && c <= 0xfe0f)) {
        return 0;
    }
    if ((c >= 0x1100 && c <= 0x115f) || (c >= 0x2e80 && c <= 0x303e)
        || (c >= 0x3041 && c <= 0x33ff) || (c >= 0x3400 && c <= 0x4dbf)
        || (c >= 0x4e00 && c <= 0x9fff) || (c >= 0xa000 && c <= 0xa4cf)
        || (c >= 0xac00 && c <= 0xd7a3) || (c >= 0xf900 && c <= 0xfaff)
        || (c >= 0xfe30 && c <= 0xfe4f) || (c >= 0xff00 && c <= 0xff60)
        || (c >= 0xffe0 && c <= 0xffe6) || (c >= 0x1f300 && c <= 0x1f64f)
        || (c >= 0x1f900 && c <= 0x1f9ff) || (c >= 0x20000 && c <= 0x3fffd)) {
        return 2;
    }
    return 1;
}

int visibleWidth(const QString &styled) {
    int width = 0;
    for (const Token &token : tokenize(styled)) width += token.width;
    return width;
}

QString fitToWidth(const QString &styled, int columns, bool pad) {
    columns = std::max(0, columns);
    QString out;
    int width = 0;
    bool styledInput = false;
    for (const Token &token : tokenize(styled)) {
        if (token.escape) {
            styledInput = true;
            out += token.text;
            continue;
        }
        if (width + token.width > columns) {
            break;
        }
        out += token.text;
        width += token.width;
    }
    if (pad && width < columns) out += QString(columns - width, QLatin1Char(' '));
    if (styledInput) out += kSgrReset;
    return out;
}

QStringList wrapPlain(const QString &plain, int columns) {
    columns = std::max(1, columns);
    QStringList out;
    for (const QString &paragraph : plain.split(QLatin1Char('\n'))) {
        QString line;
        int width = 0;
        for (QString word : paragraph.split(QLatin1Char(' '), Qt::SkipEmptyParts)) {
            int wordWidth = visibleWidth(word);
            const int needed = line.isEmpty() ? wordWidth : wordWidth + 1;
            if (width + needed <= columns) {
                if (!line.isEmpty()) line += QLatin1Char(' ');
                line += word;
                width += needed;
                continue;
            }
            if (!line.isEmpty()) {
                out.push_back(line);
                line.clear();
                width = 0;
            }
            while (wordWidth > columns) {
                QString head = fitToWidth(word, columns, false);
                if (head.isEmpty()) head = word.left(1); // a wide glyph in a 1-cell column
                out.push_back(head);
                word = word.mid(head.size());
                wordWidth = visibleWidth(word);
            }
            line = word;
            width = wordWidth;
        }
        out.push_back(line);
    }
    return out;
}

QString frameStamp(quint64 frame) {
    return QStringLiteral("F%1").arg(frame % 0x1000000, 6, 16, QLatin1Char('0')).toUpper();
}

Jank::Jank(const Options &options, QRandomGenerator *random)
    : m_options(options), m_random(random) {}

WritePlan Jank::apply(const WritePlan &plan) {
    WritePlan out;
    for (const WriteStep &step : plan) {
        if (step.bytes.isEmpty() || !chance(m_random, m_options.jankPercent)) {
            out.push_back(step);
            continue;
        }
        QVector<QByteArray> parts;
        for (qsizetype position = 0; position < step.bytes.size();) {
            const qsizetype length = boundedInt(m_random, 1, m_options.fragmentMaxBytes);
            parts.push_back(step.bytes.mid(position, length));
            position += length;
        }
        const qsizetype stallIndex =
            parts.size() > 1 ? qsizetype(m_random->bounded(quint32(parts.size() - 1))) : -1;
        const int stall = m_options.stallMaxMs > 0 ? boundedInt(m_random, 1, m_options.stallMaxMs) : 0;
        for (qsizetype index = 0; index < parts.size(); ++index) {
            WriteStep fragment{parts.at(index), index == stallIndex ? stall : 0};
            if (index + 1 == parts.size()) fragment.stallAfterMs += step.stallAfterMs;
            out.push_back(std::move(fragment));
        }
    }
    return out;
}

std::unique_ptr<ScenarioEngine> createScenario(Scenario scenario, const Options &options,
                                               QRandomGenerator *random) {
    switch (scenario) {
    case Scenario::OmpTranscript: return std::make_unique<OmpTranscriptScenario>(options, random);
    case Scenario::FullscreenChurn: return std::make_unique<FullscreenChurnScenario>(options, random);
    case Scenario::LogFlood: return std::make_unique<LogFloodScenario>(options, random);
    case Scenario::StallBurst: return std::make_unique<StallBurstScenario>(options, random);
    case Scenario::BrokenProtocol: return std::make_unique<BrokenProtocolScenario>(options, random);
    case Scenario::Chaos: return std::make_unique<ChaosScenario>(options, random);
    case Scenario::Protocol: return std::make_unique<ProtocolScenario>(options);
    }
    return nullptr;
}

QByteArray resetSequence() {
    // CAN and ST first: an unterminated escape or OSC would swallow the rest.
    return QByteArrayLiteral(
        "\x18\x1b\\\x1b[?2026l\x1b[0m\x1b[?6l\x1b[?7h\x1b[?1049l\x1b[?25h");
}

void InlineRenderer::reset() {
    m_previous.clear();
    m_previousColumns = -1;
    m_lastFull = false;
}

QByteArray InlineRenderer::render(const QStringList &lines, TerminalSize size) {
    const bool first = m_previousColumns < 0;
    const bool widthChanged = !first && m_previousColumns != size.columns;
    const qsizetype common = std::min(m_previous.size(), lines.size());
    qsizetype firstChanged = 0;
    while (firstChanged < common && m_previous.at(firstChanged) == lines.at(firstChanged)) {
        ++firstChanged;
    }
    if (!first && !widthChanged && firstChanged == lines.size()
        && lines.size() == m_previous.size()) {
        m_lastFull = false;
        return {};
    }

    const qsizetype viewportTop = std::max<qsizetype>(0, m_previous.size() - size.rows);
    const bool full = !first
        && (widthChanged || firstChanged < viewportTop || lines.size() < m_previous.size());
    QString body;
    if (first || full) {
        if (full) body = QStringLiteral("\x1b[2J\x1b[3J\x1b[H");
        body += lines.join(QStringLiteral("\r\n"));
    } else {
        const qsizetype lastRow = m_previous.size() - 1;
        if (firstChanged > lastRow) {
            body = QStringLiteral("\r\n");
        } else {
            body = QStringLiteral("\r");
            const qsizetype up = lastRow - firstChanged;
            if (up > 0) body += csi(int(up), 'A');
        }
        for (qsizetype index = firstChanged; index < lines.size(); ++index) {
            if (index > firstChanged) body += QStringLiteral("\r\n");
            body += QStringLiteral("\x1b[2K") + lines.at(index);
        }
    }
    m_previous = lines;
    m_previousColumns = size.columns;
    m_lastFull = full;
    return wrapSynchronized(body.toUtf8(), m_synchronized);
}

} // namespace spantui::stresstui
