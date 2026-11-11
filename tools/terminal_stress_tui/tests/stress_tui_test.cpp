#include "stress_tui.h"
#include "protocol_stages.h"

#include <QRegularExpression>
#include <QSet>
#include <QtTest>

using namespace spantui::stresstui;

namespace {

QStringList stampsIn(const QByteArray &bytes) {
    QStringList stamps;
    const QRegularExpression stamp(QStringLiteral("F[0-9A-F]{6}"));
    auto it = stamp.globalMatch(QString::fromUtf8(bytes));
    while (it.hasNext()) stamps.push_back(it.next().captured());
    return stamps;
}

QStringList transcript(int count, const QString &prefix = QStringLiteral("line")) {
    QStringList lines;
    for (int index = 0; index < count; ++index) lines.push_back(prefix + QString::number(index));
    return lines;
}

} // namespace

class StressTuiTest : public QObject {
    Q_OBJECT

private slots:
    void parsesOptionsAndRejectsInvalidValues() {
        Options options;
        QString error;
        QVERIFY(parseOptions({QStringLiteral("--scenario"), QStringLiteral("omp-transcript"),
                              QStringLiteral("--seed=42"), QStringLiteral("--jank"),
                              QStringLiteral("0"), QStringLiteral("--sync")},
                             &options, &error));
        QCOMPARE(options.scenario, Scenario::OmpTranscript);
        QCOMPARE(options.seed, quint64(42));
        QCOMPARE(options.jankPercent, 0);
        QVERIFY(options.synchronized);

        QVERIFY(!parseOptions({QStringLiteral("--scenario"), QStringLiteral("nope")}, &options, &error));
        QVERIFY(!parseOptions({QStringLiteral("--jank"), QStringLiteral("101")}, &options, &error));
        QVERIFY(!parseOptions({QStringLiteral("--fps")}, &options, &error));
        QVERIFY(!parseOptions({QStringLiteral("--chaos-min-ms=5000"),
                               QStringLiteral("--chaos-max-ms=1000")},
                              &options, &error));
        QCOMPARE(scenarioNames().size(), 7);
        QVERIFY(scenarioNames().contains(QStringLiteral("protocol")));
    }

    void fitsStyledAndWideTextToExactWidth() {
        QCOMPARE(visibleWidth(QStringLiteral("\x1b[31m终端ab\x1b[0m")), 6);
        // A wide glyph that would straddle the edge becomes padding.
        const QString fitted = fitToWidth(QStringLiteral("ab终端"), 5);
        QCOMPARE(visibleWidth(fitted), 5);
        QCOMPARE(fitted, QStringLiteral("ab终 "));
        const QString styled = fitToWidth(QStringLiteral("\x1b[1mhello world"), 5, false);
        QCOMPARE(visibleWidth(styled), 5);
        QVERIFY(styled.endsWith(QStringLiteral("\x1b[0m")));
    }

    void wrapsPlainTextWithinColumns() {
        const QStringList lines = wrapPlain(
            QStringLiteral("alpha beta gamma 终端渲染撕裂同步快照解析器 delta"), 10);
        QVERIFY(lines.size() > 3);
        for (const QString &line : lines) QVERIFY2(visibleWidth(line) <= 10, qPrintable(line));
    }

    void jankPreservesBytesAndStallsMidFrame() {
        Options options;
        options.jankPercent = 100;
        options.fragmentMaxBytes = 16;
        options.stallMaxMs = 50;
        QRandomGenerator random(7);
        Jank jank(options, &random);
        const QByteArray frame(1000, 'x');
        const WritePlan plan = jank.apply({{frame, 3}});
        QVERIFY(plan.size() > 1);
        QCOMPARE(concatenated(plan), frame);
        int stalls = 0;
        for (qsizetype index = 0; index + 1 < plan.size(); ++index) {
            if (plan.at(index).stallAfterMs > 0) ++stalls;
            QVERIFY(plan.at(index).bytes.size() <= 16);
        }
        QCOMPARE(stalls, 1);
        QCOMPARE(plan.back().stallAfterMs, 3);

        options.jankPercent = 0;
        Jank calm(options, &random);
        QCOMPARE(calm.apply({{frame, 0}}).size(), 1);
    }

    void inlineRendererRewritesFromFirstChangedLine() {
        InlineRenderer renderer(false);
        const TerminalSize size{80, 10};
        QStringList lines = transcript(5);
        const QByteArray first = renderer.render(lines, size);
        QVERIFY(!first.contains("\x1b[3J"));
        QVERIFY(first.startsWith("line0"));

        QVERIFY(renderer.render(lines, size).isEmpty());

        lines[3] = QStringLiteral("changed");
        const QByteArray diff = renderer.render(lines, size);
        QCOMPARE(diff, QByteArray("\r\x1b[1A\x1b[2Kchanged\r\n\x1b[2Kline4"));
        QVERIFY(!renderer.lastRenderWasFullReprint());

        lines.push_back(QStringLiteral("appended"));
        QCOMPARE(renderer.render(lines, size), QByteArray("\r\n\x1b[2Kappended"));
    }

    void inlineRendererReprintsWhenChangeIsAboveViewport() {
        InlineRenderer renderer(true);
        const TerminalSize size{80, 10};
        QStringList lines = transcript(40);
        renderer.render(lines, size);
        lines[2] = QStringLiteral("edited far above");
        const QByteArray reprint = renderer.render(lines, size);
        QVERIFY(renderer.lastRenderWasFullReprint());
        QVERIFY(reprint.startsWith("\x1b[?2026h\x1b[2J\x1b[3J\x1b[H"));
        QVERIFY(reprint.endsWith("\x1b[?2026l"));
        QVERIFY(reprint.contains("edited far above"));

        renderer.render(lines, TerminalSize{100, 10});
        QVERIFY(renderer.lastRenderWasFullReprint());
    }

    void fullscreenFrameStampsEveryRowAlike() {
        Options options;
        QRandomGenerator random(1);
        auto scenario = createScenario(Scenario::FullscreenChurn, options, &random);
        const TerminalSize size{60, 12};
        scenario->enter(size);
        for (int frame = 0; frame < 3; ++frame) {
            const QByteArray bytes = concatenated(scenario->tick(size));
            const QStringList stamps = stampsIn(bytes);
            QCOMPARE(stamps.size(), size.rows);
            QCOMPARE(QSet<QString>(stamps.begin(), stamps.end()).size(), 1);
            QVERIFY(!bytes.contains("\x1b[?2026"));
        }
    }

    void sameSeedProducesSameStream() {
        Options options;
        options.scenario = Scenario::OmpTranscript;
        auto run = [&options]() {
            QRandomGenerator random(options.seed);
            Jank jank(options, &random);
            auto scenario = createScenario(options.scenario, options, &random);
            QByteArray bytes = scenario->enter({80, 24});
            for (int tick = 0; tick < 300; ++tick) {
                bytes += concatenated(jank.apply(scenario->tick({80, 24})));
            }
            return bytes;
        };
        const QByteArray first = run();
        QCOMPARE(run(), first);
        QVERIFY(first.contains("\x1b[3J"));
    }

    void brokenProtocolLosesSyncResetAndSplitsEscapes() {
        Options options;
        QRandomGenerator random(3);
        auto scenario = createScenario(Scenario::BrokenProtocol, options, &random);
        const TerminalSize size{60, 12};
        scenario->enter(size);
        const QByteArray lostReset = concatenated(scenario->tick(size));
        QVERIFY(lostReset.startsWith("\x1b[?2026h"));
        QVERIFY(!lostReset.contains("\x1b[?2026l"));
        scenario->tick(size);
        const WritePlan split = scenario->tick(size);
        QVERIFY(split.size() > size.rows);
        bool endsInsideEscape = false;
        for (const WriteStep &step : split) endsInsideEscape |= step.bytes.endsWith('\x1b');
        QVERIFY(endsInsideEscape);
        QVERIFY(scenario->leave().startsWith("\x18\x1b\\"));
    }

    void protocolStagesEmitTheirProtocolFamilies() {
        const auto names = protocol::stageNames();
        QCOMPARE(names.size(), std::size_t(protocol::Stage::Count));
        QSet<QString> unique;
        for (const auto name : names) unique.insert(QString::fromStdString(std::string(name)));
        QCOMPARE(unique.size(), qsizetype(names.size()));

        const TerminalSize size{100, 30};
        const auto frame = [&](protocol::Stage stage, std::uint64_t index = 0,
                               bool first = true) {
            return protocol::renderFrame({stage, size, index, first, true});
        };
        for (std::size_t index = 0; index < std::size_t(protocol::Stage::Count); ++index) {
            const std::string bytes = frame(protocol::Stage(index));
            QVERIFY(!bytes.empty());
            QVERIFY(bytes.rfind("\x1b[?2026h", 0) == 0);
        }

        const std::string legacy = frame(protocol::Stage::LegacyCharsets, 12, false);
        QVERIFY(legacy.find("\x1b(0") != std::string::npos);
        QVERIFY(legacy.find("\x1b[?2l") != std::string::npos);
        QVERIFY(legacy.find("\x1b#6") != std::string::npos);

        const std::string editing = frame(protocol::Stage::CursorAndEditing);
        QVERIFY(editing.find("\x1b[3@") != std::string::npos);
        QVERIFY(editing.find("\x1b[3P") != std::string::npos);
        QVERIFY(editing.find("\x1b[4X") != std::string::npos);

        const std::string text = frame(protocol::Stage::LegacySgr);
        QVERIFY(text.find("\x1b[4:3m") != std::string::npos);
        QVERIFY(text.find("\x1b[5m") != std::string::npos);
        QVERIFY(text.find(u8"中文") != std::string::npos);

        const std::string colors = frame(protocol::Stage::ExtendedColor, 3);
        QVERIFY(colors.find("\x1b[48;5;") != std::string::npos);
        QVERIFY(colors.find("\x1b[48;2;") != std::string::npos);

        QVERIFY(frame(protocol::Stage::MarginsAndScrolling).find(";27r") != std::string::npos);

        const std::string modes = frame(protocol::Stage::ModernModes);
        QVERIFY(modes.find("\x1b[?2004h") != std::string::npos);
        QVERIFY(modes.find("\x1b[>3u") != std::string::npos);

        const std::string links = frame(protocol::Stage::ModernOsc);
        QVERIFY(links.find("\x1b]8;") != std::string::npos);
        QVERIFY(links.find("\x1b[58;2;") != std::string::npos);

        const std::string graphics = frame(protocol::Stage::InlineGraphics);
        QVERIFY(graphics.find("\x1b_Ga=T,f=32") != std::string::npos);
        QVERIFY(graphics.find("\x1b]1337;File=") != std::string::npos);
        QVERIFY(graphics.find("\x1bPq") != std::string::npos);
    }

    void protocolScenarioWritesWholeFramesAndRestoresModes() {
        Options options;
        QString error;
        QVERIFY(parseOptions({QStringLiteral("--scenario=protocol"),
                              QStringLiteral("--protocol-stage"), QStringLiteral("modern-osc")},
                             &options, &error));
        QCOMPARE(options.scenario, Scenario::Protocol);
        QCOMPARE(options.protocolStage, int(protocol::Stage::ModernOsc));
        QVERIFY(!parseOptions({QStringLiteral("--protocol-stage"), QStringLiteral("nope")},
                              &options, &error));

        QVERIFY(parseOptions({QStringLiteral("--scenario=protocol"),
                              QStringLiteral("--protocol-stage=modern-osc")},
                             &options, &error));
        QRandomGenerator random(1);
        auto scenario = createScenario(Scenario::Protocol, options, &random);
        QVERIFY(!scenario->acceptsJank());
        const TerminalSize size{80, 24};
        QVERIFY(scenario->enter(size).contains("\x1b[?1049h"));
        const WritePlan first = scenario->tick(size);
        QCOMPARE(first.size(), qsizetype(1));
        QVERIFY(first.front().bytes.contains("\x1b]8;"));
        // Later frames of a pinned stage repaint only when the size changes.
        QVERIFY(!concatenated(scenario->tick(size)).contains("\x1b[2J"));
        QVERIFY(concatenated(scenario->tick({100, 30})).contains("\x1b[2J"));
        const QByteArray leave = scenario->leave();
        QVERIFY(leave.contains("\x1b[r"));
        QVERIFY(leave.contains("\x1b[?2004l"));
        QVERIFY(leave.endsWith("\x1b[?1049l"));
    }
};

QTEST_GUILESS_MAIN(StressTuiTest)
#include "stress_tui_test.moc"
