#include "terminal/api/terminal_types.h"
#include "terminal/core/ghostty_engine.h"

#include <QBuffer>
#include <QImage>
#include <QTest>

#include <algorithm>
#include <utility>

// GhosttyEngine is SpanTui's adapter around libghostty-vt. These cases check
// what the adapter adds or decides: how cells, colours and rows reach a
// Frame, which replies the child gets, how keys are encoded (win32-input-mode
// included), and the history, selection, search and graphics the view relies
// on. libghostty's own VT parsing is not re-tested here.

using namespace span::terminal;

namespace {

constexpr Qt::KeyboardModifiers None = Qt::NoModifier;

QString lineText(const Line &line)
{
    QString text;
    for (const Cell &cell : line.cells) {
        if (cell.continuation) continue;
        text.append(cell.text.isEmpty() ? QStringLiteral(" ") : cell.text);
    }
    while (text.endsWith(QLatin1Char(' '))) text.chop(1);
    return text;
}

// The viewport's rows, overscan left out.
QStringList viewportText(const Frame &frame)
{
    QStringList rows;
    for (int row = 0; row < frame.rows && frame.viewportRow + row < frame.lines.size(); ++row)
        rows.append(lineText(frame.lines.at(frame.viewportRow + row)));
    return rows;
}

const Line *findLine(const Frame &frame, const QString &text)
{
    const auto found = std::find_if(frame.lines.cbegin(), frame.lines.cend(),
                                    [&](const Line &line) { return lineText(line) == text; });
    return found == frame.lines.cend() ? nullptr : &*found;
}

QByteArray numberedLines(int from, int to)
{
    QByteArray bytes;
    for (int index = from; index < to; ++index) bytes += "line " + QByteArray::number(index) + "\r\n";
    return bytes;
}

QByteArray keyRecord(int virtualKey, int scanCode, int unit, bool down, int state)
{
    return "\x1b[" + QByteArray::number(virtualKey) + ';' + QByteArray::number(scanCode) + ';'
        + QByteArray::number(unit) + ';' + (down ? '1' : '0') + ';' + QByteArray::number(state) + ";1_";
}

QByteArray characterRecords(int unit)
{
    return keyRecord(0, 0, unit, true, 0) + keyRecord(0, 0, unit, false, 0);
}

// The Up arrow as Qt's Windows plugin reports it.
TerminalKeyStroke physicalUp(Qt::KeyboardModifiers modifiers = None)
{
    TerminalKeyStroke stroke = TerminalKeyStroke::named(TerminalKey::Up, modifiers);
    stroke.native = {0x26, 0x48,
                     TerminalControlKeyState::EnhancedKey
                         | (modifiers & Qt::ControlModifier ? TerminalControlKeyState::LeftCtrl : 0),
                     QString()};
    return stroke;
}

TerminalSelectionEvent selectionAt(TerminalSelectionEvent::Kind kind, int row, int column, qreal x, qint64 timeMs = 0)
{
    TerminalSelectionEvent event;
    event.kind = kind;
    event.row = row;
    event.column = column;
    event.x = x;
    event.y = row * 20 + 10;
    event.timeMs = timeMs;
    return event;
}

// An engine whose replies to the child, clipboard writes, titles and host
// events are collected.
struct Terminal {
    GhosttyEngine engine;
    QByteArray output;
    QStringList clipboard;
    QStringList titles;
    QList<TerminalHostEvent> events;

    explicit Terminal(int columns = 20, int rows = 4) : engine(columns, rows)
    {
        engine.setOutputCallback([this](const QByteArray &bytes) { output += bytes; });
        engine.setClipboardCallback([this](const QString &text) { clipboard.append(text); });
        engine.setTitleCallback([this](const QString &title) { titles.append(title); });
        engine.setHostEventCallback([this](const TerminalHostEvent &event) { events.append(event); });
        engine.setCellPixelSize(QSize(10, 20));
    }

    QByteArray feed(const QByteArray &bytes)
    {
        output.clear();
        engine.feed(bytes);
        return std::exchange(output, {});
    }

    QByteArray send(const QList<TerminalKeyStroke> &strokes)
    {
        output.clear();
        for (const TerminalKeyStroke &stroke : strokes) engine.sendKeyStroke(stroke);
        return std::exchange(output, {});
    }

    void finishSearch()
    {
        for (int step = 0; step < 1000 && engine.advanceSearch(); ++step) {}
    }
};

} // namespace

class GhosttyEngineTest final : public QObject {
    Q_OBJECT

private slots:
    void readsTextAndWideCells();
    void keepsThemeColorsSemantic();
    void childColorsOverrideTheTheme();
    void carriesTextAttributes();
    void marksSoftWrapsAndHyperlinks();
    void keepsRowIdentityWhileRowsScroll();
    void scrollsTheViewportThroughHistory();
    void reflowsHistoryWhenColumnsChange();
    void clearDisplayDropsHistory();
    void limitsHistoryRows();
    void alternateScreenHasNoHistoryOrPromptMarks();
    void followsDefaultCursorShapeUntilTheChildSetsOne();
    void deliversClipboardWritesButNeverReads();
    void reportsColorSchemeOnQueryAndChange();
    void deliversTitleBellNotificationAndProgress();
    void answersSizeAndVersionButNotTitleReports();
    void jumpsBetweenPromptMarks();
    void placesInlineKittyImages();
    void refusesKittyFileMedia();
    void searchesHistoryIgnoringAsciiCase();
    void selectsByGestureWordAndAll();
    void encodesLegacyAndKittyKeys();
    void win32ModeSendsKeyRecords();
    void negotiatedVtProtocolOverridesWin32Mode();
    void wrapsBracketedPaste();
    void forwardsMouseAndFocusReports();
};

void GhosttyEngineTest::readsTextAndWideCells()
{
    Terminal terminal;
    terminal.feed("a\xe4\xb8\xad"
                  "b");
    const Frame frame = terminal.engine.frame();
    QCOMPARE(frame.columns, 20);
    QCOMPARE(frame.rows, 4);
    const Line &line = frame.lines.at(frame.viewportRow);
    QCOMPARE(line.cells.size(), 20);
    QCOMPARE(line.cells.at(0).text, QStringLiteral("a"));
    QCOMPARE(line.cells.at(1).text, QString(QChar(0x4e2d)));
    QCOMPARE(line.cells.at(1).width, 2);
    QVERIFY(line.cells.at(2).continuation);
    QCOMPARE(line.cells.at(3).text, QStringLiteral("b"));
    QVERIFY(!line.id.isNull());
    QVERIFY(line.revision > 0);
    QCOMPARE(frame.cursor.row, 0);
    QCOMPARE(frame.cursor.column, 4);
}

void GhosttyEngineTest::keepsThemeColorsSemantic()
{
    Terminal terminal;
    QVector<QColor> palette;
    for (int index = 0; index < 16; ++index) palette.append(QColor(index * 10, index * 5, 200));
    terminal.engine.setDefaultColors(Qt::white, Qt::black);
    terminal.engine.setAnsiPalette(palette);
    terminal.feed("\x1b[31mr\x1b[0m\x1b[38;5;200mx\x1b[38;2;1;2;3mt\x1b[0md\x1b[44m \x1b[0m");
    const Line &line = terminal.engine.frame().lines.at(0);

    // Theme slots stay palette indices, so a theme switch recolours them.
    QCOMPARE(line.cells.at(0).style.foregroundPaletteIndex, 1);
    QVERIFY(!line.cells.at(0).style.defaultForeground);
    // The 256-colour cube and true colour are plain colours.
    QCOMPARE(line.cells.at(1).style.foregroundPaletteIndex, -1);
    QVERIFY(line.cells.at(1).style.foreground.isValid());
    QCOMPARE(line.cells.at(2).style.foreground, QColor(1, 2, 3));
    QCOMPARE(line.cells.at(2).style.foregroundPaletteIndex, -1);
    // Default colours stay default.
    QVERIFY(line.cells.at(3).style.defaultForeground);
    QVERIFY(line.cells.at(3).style.defaultBackground);
    QCOMPARE(line.cells.at(4).style.backgroundPaletteIndex, 4);
}

void GhosttyEngineTest::childColorsOverrideTheTheme()
{
    Terminal terminal;
    terminal.engine.setDefaultColors(Qt::white, Qt::black);
    // OSC 4 redefines ANSI red; OSC 10 the default foreground.
    terminal.feed("\x1b]4;1;rgb:12/34/56\x07\x1b]10;rgb:ff/80/00\x07\x1b[31mr\x1b[0md");
    const Line &line = terminal.engine.frame().lines.at(0);
    QCOMPARE(line.cells.at(0).style.foregroundPaletteIndex, -1);
    QCOMPARE(line.cells.at(0).style.foreground, QColor(0x12, 0x34, 0x56));
    QVERIFY(!line.cells.at(1).style.defaultForeground);
    QCOMPARE(line.cells.at(1).style.foreground, QColor(0xff, 0x80, 0x00));

    // Queries answer with the colours in force.
    const QByteArray reply = terminal.feed("\x1b]10;?\x07\x1b]11;?\x07");
    QVERIFY2(reply.contains("10;rgb:ffff/8080/0000"), reply.constData());
    QVERIFY2(reply.contains("11;rgb:0000/0000/0000"), reply.constData());
}

void GhosttyEngineTest::carriesTextAttributes()
{
    Terminal terminal;
    terminal.feed("\x1b[1ma\x1b[0m\x1b[2mb\x1b[0m\x1b[3mc\x1b[0m\x1b[4:3md\x1b[0m"
                  "\x1b[4;58;2;255;0;0me\x1b[0m\x1b[53mf\x1b[0m\x1b[9mg\x1b[0m\x1b[5mh\x1b[0m"
                  "\x1b[7mi\x1b[0m\x1b[8mj\x1b[0m");
    const Line &line = terminal.engine.frame().lines.at(0);
    QVERIFY(line.cells.at(0).style.bold);
    QVERIFY(line.cells.at(1).style.faint);
    QVERIFY(line.cells.at(2).style.italic);
    QCOMPARE(line.cells.at(3).style.underline, UnderlineStyle::Curly);
    QCOMPARE(line.cells.at(4).style.underline, UnderlineStyle::Single);
    QCOMPARE(line.cells.at(4).style.underlineColor, QColor(255, 0, 0));
    QVERIFY(line.cells.at(5).style.overline);
    QVERIFY(line.cells.at(6).style.strike);
    QVERIFY(line.cells.at(7).style.blink);
    // Reverse and conceal stay attributes until paint.
    QVERIFY(line.cells.at(8).style.reverse);
    QVERIFY(line.cells.at(8).style.defaultForeground);
    QVERIFY(line.cells.at(9).style.conceal);
}

void GhosttyEngineTest::marksSoftWrapsAndHyperlinks()
{
    Terminal terminal(10, 4);
    terminal.feed("abcdefghijKL\r\n\x1b]8;;https://example.com\x1b\\link\x1b]8;;\x1b\\ plain");
    const Frame frame = terminal.engine.frame();
    QVERIFY(frame.lines.at(0).softWrapped);
    QVERIFY(!frame.lines.at(1).softWrapped);
    QCOMPARE(lineText(frame.lines.at(1)), QStringLiteral("KL"));
    const Line &links = frame.lines.at(2);
    QCOMPARE(links.cells.at(0).style.hyperlink, QStringLiteral("https://example.com"));
    QCOMPARE(links.cells.at(3).style.hyperlink, QStringLiteral("https://example.com"));
    QVERIFY(links.cells.at(5).style.hyperlink.isEmpty());
}

void GhosttyEngineTest::keepsRowIdentityWhileRowsScroll()
{
    Terminal terminal(20, 4);
    terminal.feed(numberedLines(0, 3));
    const Frame first = terminal.engine.frame();
    const Line *before = findLine(first, QStringLiteral("line 1"));
    QVERIFY(before);

    // Scrolled up into history and back into the overscan: same row, same
    // content, same revision.
    terminal.feed(numberedLines(3, 5));
    const Frame scrolled = terminal.engine.frame();
    const Line *after = findLine(scrolled, QStringLiteral("line 1"));
    QVERIFY(after);
    QCOMPARE(after->id, before->id);
    QCOMPARE(after->revision, before->revision);

    // Rewriting a row keeps its id and gives it a new revision.
    terminal.feed("\x1b[1;1Hchanged\x1b[K");
    const Frame rewritten = terminal.engine.frame();
    const Line &top = rewritten.lines.at(rewritten.viewportRow);
    const auto previousTop = std::find_if(scrolled.lines.cbegin(), scrolled.lines.cend(),
                                          [&](const Line &line) { return line.id == top.id; });
    QVERIFY(previousTop != scrolled.lines.cend());
    QVERIFY(previousTop->revision != top.revision);
    QCOMPARE(lineText(top), QStringLiteral("changed"));
}

void GhosttyEngineTest::scrollsTheViewportThroughHistory()
{
    Terminal terminal(20, 4);
    terminal.feed(numberedLines(0, 12));
    const Frame bottom = terminal.engine.frame();
    const qint64 history = terminal.engine.scrollbackRows();
    QVERIFY(history > 0);
    QCOMPARE(bottom.scrollbar.total, history + 4);
    QCOMPARE(bottom.scrollbar.rows, 4);
    QVERIFY(bottom.scrollbar.atBottom());
    // A screen of overscan above the live screen, none below it.
    QCOMPARE(bottom.viewportRow, 4);
    QCOMPARE(bottom.lines.size(), 8);
    QVERIFY(bottom.cursor.visible);

    terminal.engine.scrollViewportTo(0);
    const Frame top = terminal.engine.frame();
    QCOMPARE(top.scrollbar.offset, qint64(0));
    QVERIFY(!top.scrollbar.atBottom());
    QCOMPARE(top.viewportRow, 0);
    QCOMPARE(viewportText(top).first(), QStringLiteral("line 0"));
    QCOMPARE(top.lines.size(), 8);

    terminal.engine.scrollViewportTo(3);
    QCOMPARE(viewportText(terminal.engine.frame()).first(), QStringLiteral("line 3"));

    // Narrow overscan: one row each side, for a fractional top.
    terminal.engine.setWideOverscan(false);
    const Frame narrow = terminal.engine.frame();
    QCOMPARE(narrow.viewportRow, 1);
    QCOMPARE(narrow.lines.size(), 6);
    QCOMPARE(viewportText(narrow).first(), QStringLiteral("line 3"));
    // Wide again: the three history rows above, a screen below.
    terminal.engine.setWideOverscan(true);
    QCOMPARE(terminal.engine.frame().lines.size(), 11);

    terminal.engine.scrollViewportToBottom();
    QVERIFY(terminal.engine.frame().scrollbar.atBottom());
}

void GhosttyEngineTest::reflowsHistoryWhenColumnsChange()
{
    // A long line wrapped at 10 columns becomes one row again at 30, even
    // after it scrolled into history.
    Terminal terminal(10, 3);
    terminal.feed("abcdefghijKLMNO\r\n" + numberedLines(0, 6));
    terminal.engine.resize(30, 3);
    terminal.engine.scrollViewportTo(0);
    const Frame frame = terminal.engine.frame();
    QCOMPARE(frame.columns, 30);
    QCOMPARE(viewportText(frame).first(), QStringLiteral("abcdefghijKLMNO"));
    QVERIFY(!frame.lines.at(frame.viewportRow).softWrapped);
}

void GhosttyEngineTest::clearDisplayDropsHistory()
{
    // ConPTY's clear keeps the cursor's row as the top row; the engine's
    // matches it, or the shell's next output misses its prompt.
    Terminal terminal(20, 4);
    terminal.feed(numberedLines(0, 20) + "PS> ls");
    QVERIFY(terminal.engine.scrollbackRows() > 0);
    terminal.engine.clearDisplay();
    QCOMPARE(terminal.engine.scrollbackRows(), qint64(0));
    const Frame frame = terminal.engine.frame();
    QCOMPARE(frame.lines.size(), 4);
    QCOMPARE(viewportText(frame), (QStringList{QStringLiteral("PS> ls"), QString(), QString(), QString()}));
    QCOMPARE(frame.cursor.row, 0);
    QCOMPARE(frame.cursor.column, 6);
}

void GhosttyEngineTest::limitsHistoryRows()
{
    // libghostty prunes whole pages, so somewhat more than the limit
    // survives, but far from everything.
    constexpr int kLines = 50'000;
    Terminal limited(20, 4);
    limited.engine.setMaxScrollback(100);
    limited.feed(numberedLines(0, kLines));
    QVERIFY(limited.engine.scrollbackRows() >= 100);
    QVERIFY(limited.engine.scrollbackRows() < kLines / 2);

    Terminal unlimited(20, 4);
    unlimited.engine.setMaxScrollback(-1);
    unlimited.feed(numberedLines(0, kLines));
    QVERIFY(unlimited.engine.scrollbackRows() >= kLines - 4);
}

void GhosttyEngineTest::alternateScreenHasNoHistoryOrPromptMarks()
{
    Terminal terminal(20, 4);
    terminal.feed("\x1b]133;A\x07$ \r\n");
    QVERIFY(terminal.engine.frame().promptMarks);
    terminal.feed("\x1b[?1049h" + numberedLines(0, 20));
    const Frame frame = terminal.engine.frame();
    QVERIFY(frame.alternateScreen);
    QVERIFY(!frame.promptMarks);
    QCOMPARE(frame.scrollbar.total, qint64(4));
    terminal.feed("\x1b[?1049l");
    QVERIFY(!terminal.engine.frame().alternateScreen);
    QVERIFY(terminal.engine.frame().promptMarks);
}

void GhosttyEngineTest::followsDefaultCursorShapeUntilTheChildSetsOne()
{
    Terminal terminal;
    QCOMPARE(terminal.engine.frame().cursor.shape, CursorShape::Block);
    terminal.engine.setDefaultCursorShape(CursorShape::Bar);
    QCOMPARE(terminal.engine.frame().cursor.shape, CursorShape::Bar);
    terminal.feed("\x1b[4 q");
    QCOMPARE(terminal.engine.frame().cursor.shape, CursorShape::Underline);
    QVERIFY(!terminal.engine.frame().cursor.blink);
    // DECSCUSR 0 returns to the user's shape.
    terminal.feed("\x1b[0 q");
    QCOMPARE(terminal.engine.frame().cursor.shape, CursorShape::Bar);
}

void GhosttyEngineTest::deliversClipboardWritesButNeverReads()
{
    Terminal terminal;
    QVERIFY(terminal.feed("\x1b]52;c;" + QByteArray("hello").toBase64() + "\x07").isEmpty());
    QCOMPARE(terminal.clipboard, QStringList{QStringLiteral("hello")});
    const QByteArray reply = terminal.feed("\x1b]52;c;?\x07");
    QVERIFY2(!reply.contains("aGVsbG8"), reply.constData());
}

void GhosttyEngineTest::reportsColorSchemeOnQueryAndChange()
{
    Terminal terminal;
    terminal.engine.setDefaultColors(Qt::white, Qt::black);
    QCOMPARE(terminal.feed("\x1b[?996n"), QByteArray("\x1b[?997;1n"));

    // Mode 2031 reports a change of the surface's darkness unasked.
    terminal.feed("\x1b[?2031h");
    terminal.output.clear();
    terminal.engine.setDefaultColors(Qt::black, Qt::white);
    QCOMPARE(std::exchange(terminal.output, {}), QByteArray("\x1b[?997;2n"));
    // Same darkness, nothing to report.
    terminal.engine.setDefaultColors(Qt::darkGray, QColor(250, 250, 250));
    QVERIFY(terminal.output.isEmpty());
}

void GhosttyEngineTest::deliversTitleBellNotificationAndProgress()
{
    Terminal terminal;
    terminal.feed("\x1b]2;my title\x07\x07\x1b]777;notify;Build;done\x07\x1b]9;4;1;40\x07");
    QVERIFY(terminal.titles.contains(QStringLiteral("my title")));
    const auto has = [&](TerminalHostEvent::Kind kind) {
        return std::find_if(terminal.events.cbegin(), terminal.events.cend(),
                            [&](const TerminalHostEvent &event) { return event.kind == kind; });
    };
    QVERIFY(has(TerminalHostEvent::Kind::Bell) != terminal.events.cend());
    const auto notification = has(TerminalHostEvent::Kind::Notification);
    QVERIFY(notification != terminal.events.cend());
    QCOMPARE(notification->title, QStringLiteral("Build"));
    QCOMPARE(notification->text, QStringLiteral("done"));
    const auto progress = has(TerminalHostEvent::Kind::Progress);
    QVERIFY(progress != terminal.events.cend());
    QCOMPARE(progress->progress, TerminalProgressState::Normal);
    QCOMPARE(progress->progressValue, 40);
}

void GhosttyEngineTest::answersSizeAndVersionButNotTitleReports()
{
    Terminal terminal(20, 4);
    QCOMPARE(terminal.feed("\x1b[18t"), QByteArray("\x1b[8;4;20t"));
    QCOMPARE(terminal.feed("\x1b[14t"), QByteArray("\x1b[4;80;200t"));
    QVERIFY(terminal.feed("\x1b[>q").contains("SpanTui"));
    // A title report would type child-chosen text back into the shell.
    terminal.feed("\x1b]2;rm -rf\x07");
    QVERIFY(terminal.feed("\x1b[21t").isEmpty());
}

void GhosttyEngineTest::jumpsBetweenPromptMarks()
{
    Terminal terminal(20, 4);
    QVERIFY(!terminal.engine.frame().promptMarks);
    terminal.feed("\x1b]133;A\x07"
                  "first$ \x1b]133;B\x07\r\n"
                  + numberedLines(0, 10) + "\x1b]133;A\x07second$ \x1b]133;B\x07\r\n" + numberedLines(10, 20));
    QVERIFY(terminal.engine.frame().promptMarks);

    terminal.engine.scrollViewportByPrompts(-1);
    const Frame previous = terminal.engine.frame();
    QVERIFY(!previous.scrollbar.atBottom());
    QCOMPARE(viewportText(previous).first(), QStringLiteral("second$"));

    terminal.engine.scrollViewportByPrompts(-1);
    QCOMPARE(viewportText(terminal.engine.frame()).first(), QStringLiteral("first$"));
}

void GhosttyEngineTest::placesInlineKittyImages()
{
    Terminal terminal(20, 4);
    const quint64 revision = terminal.engine.graphicsRevision();
    // A 2x2 RGB image, transmitted inline and displayed at the cursor.
    const QByteArray pixels("\xff\x00\x00\x00\xff\x00\x00\x00\xff\xff\xff\xff", 12);
    terminal.feed("\x1b[2;3H\x1b_Ga=T,f=24,s=2,v=2,t=d,q=2;" + pixels.toBase64() + "\x1b\\");
    const Frame frame = terminal.engine.frame();
    QCOMPARE(frame.graphics.size(), 1);
    const GraphicPlacement &placement = frame.graphics.constFirst();
    QCOMPARE(placement.row, 1);
    QCOMPARE(placement.column, 2);
    QVERIFY(placement.resource);
    QCOMPARE(placement.resource->image.size(), QSize(2, 2));
    QCOMPARE(placement.resource->image.pixelColor(0, 0), QColor(255, 0, 0));
    QCOMPARE(placement.pixelSize, QSize(2, 2));
    QVERIFY(terminal.engine.graphicsRevision() != revision);

    // PNG payloads decode through Qt.
    QImage png(3, 1, QImage::Format_RGBA8888);
    png.fill(QColor(0, 0, 255));
    QByteArray encoded;
    QBuffer buffer(&encoded);
    buffer.open(QIODevice::WriteOnly);
    QVERIFY(png.save(&buffer, "PNG"));
    terminal.feed("\x1b[4;1H\x1b_Ga=T,f=100,t=d,q=2;" + encoded.toBase64() + "\x1b\\");
    const Frame withPng = terminal.engine.frame();
    const auto decoded =
        std::find_if(withPng.graphics.cbegin(), withPng.graphics.cend(), [](const GraphicPlacement &graphic) {
            return graphic.resource && graphic.resource->image.size() == QSize(3, 1);
        });
    QVERIFY(decoded != withPng.graphics.cend());
}

void GhosttyEngineTest::refusesKittyFileMedia()
{
    // File media would let a child make the terminal read paths of its
    // choosing.
    Terminal terminal(20, 4);
    terminal.feed("\x1b_Ga=T,f=100,t=f,q=2;" + QByteArray("C:\\Windows\\win.ini").toBase64() + "\x1b\\");
    terminal.feed("\x1b_Ga=T,f=100,t=t,q=2;" + QByteArray("C:\\Windows\\win.ini").toBase64() + "\x1b\\");
    QVERIFY(terminal.engine.frame().graphics.isEmpty());
}

void GhosttyEngineTest::searchesHistoryIgnoringAsciiCase()
{
    Terminal terminal(20, 4);
    terminal.feed("a needle here\r\n" + numberedLines(0, 10) + "NEEDLE again\r\n");
    terminal.engine.setSearchNeedle(QStringLiteral("Needle"));
    terminal.finishSearch();
    TerminalSearchStatus status = terminal.engine.searchStatus();
    QCOMPARE(status.needle, QStringLiteral("Needle"));
    QVERIFY(status.complete);
    QCOMPARE(status.total, 2);

    // Selecting the newest, then the older match scrolls it into view and
    // marks it current.
    terminal.engine.selectSearchMatch(false);
    terminal.engine.selectSearchMatch(false);
    terminal.finishSearch();
    const Frame frame = terminal.engine.frame();
    QVERIFY(!frame.scrollbar.atBottom());
    const auto current = std::find_if(frame.searchMatches.cbegin(), frame.searchMatches.cend(),
                                      [](const TerminalHighlight &match) { return match.current; });
    QVERIFY(current != frame.searchMatches.cend());
    QCOMPARE(lineText(frame.lines.at(current->line)), QStringLiteral("a needle here"));
    QCOMPARE(current->startColumn, 2);
    QCOMPARE(current->endColumn, 8);
    QVERIFY(terminal.engine.searchStatus().selected >= 0);

    terminal.engine.setSearchNeedle(QString());
    QCOMPARE(terminal.engine.searchStatus().total, 0);
    QVERIFY(terminal.engine.frame().searchMatches.isEmpty());
}

void GhosttyEngineTest::selectsByGestureWordAndAll()
{
    using Kind = TerminalSelectionEvent::Kind;
    Terminal terminal(20, 4);
    terminal.feed("hello world\r\nnext");

    // Press in the left half of "h", drag to the right half of "o".
    terminal.engine.selectionEvent(selectionAt(Kind::Press, 0, 0, 2));
    terminal.engine.selectionEvent(selectionAt(Kind::Drag, 0, 4, 48));
    terminal.engine.selectionEvent(selectionAt(Kind::Release, 0, 4, 48));
    QCOMPARE(terminal.engine.selectedText(), QStringLiteral("hello"));
    const Frame selected = terminal.engine.frame();
    QVERIFY(selected.hasSelection);
    QCOMPARE(selected.selection.size(), 1);
    QCOMPARE(selected.selection.constFirst().line, selected.viewportRow);
    QCOMPARE(selected.selection.constFirst().startColumn, 0);
    QCOMPARE(selected.selection.constFirst().endColumn, 5);

    // A double click selects the word.
    terminal.engine.selectionEvent(selectionAt(Kind::Press, 0, 7, 75, 1000));
    terminal.engine.selectionEvent(selectionAt(Kind::Release, 0, 7, 75, 1000));
    terminal.engine.selectionEvent(selectionAt(Kind::Press, 0, 7, 75, 1100));
    terminal.engine.selectionEvent(selectionAt(Kind::Release, 0, 7, 75, 1100));
    QCOMPARE(terminal.engine.selectedText(), QStringLiteral("world"));

    terminal.engine.selectAll();
    QVERIFY(terminal.engine.selectedText().contains(QStringLiteral("hello world")));
    QVERIFY(terminal.engine.selectedText().contains(QStringLiteral("next")));

    terminal.engine.clearSelection();
    QVERIFY(!terminal.engine.frame().hasSelection);
    QVERIFY(terminal.engine.selectedText().isEmpty());
}

void GhosttyEngineTest::encodesLegacyAndKittyKeys()
{
    Terminal terminal;
    const TerminalKeyStroke up = TerminalKeyStroke::named(TerminalKey::Up);
    QCOMPARE(terminal.send({up, TerminalKeyStroke::named(TerminalKey::Enter)}), QByteArray("\x1b[A\r"));
    QCOMPARE(terminal.send({TerminalKeyStroke::character(QStringLiteral("a"), Qt::ControlModifier)}),
             QByteArray("\x01"));
    QCOMPARE(terminal.send({TerminalKeyStroke::character(QStringLiteral("x"), Qt::AltModifier)}), QByteArray("\x1bx"));
    // Without a negotiated protocol a release sends nothing.
    QVERIFY(terminal.send({TerminalKeyStroke::named(TerminalKey::Up, None, 0, TerminalKeyEvent::Release)}).isEmpty());
    // Shift that produced the character is not reported again.
    QCOMPARE(terminal.send({TerminalKeyStroke::character(QStringLiteral("\""), Qt::ShiftModifier,
                                                         TerminalKeyEvent::Press, {'\'', '"', 0})}),
             QByteArray("\""));

    // DECCKM switches the cursor keys to SS3.
    terminal.feed("\x1b[?1h");
    QCOMPARE(terminal.send({up}), QByteArray("\x1bOA"));
    terminal.feed("\x1b[?1l");

    // Kitty: flags are queried, pushed and popped; disambiguated keys are
    // CSI u.
    QCOMPARE(terminal.feed("\x1b[?u"), QByteArray("\x1b[?0u"));
    QCOMPARE(terminal.feed("\x1b[>1u\x1b[?u"), QByteArray("\x1b[?1u"));
    QCOMPARE(terminal.send({TerminalKeyStroke::character(QStringLiteral("a"), Qt::ControlModifier)}),
             QByteArray("\x1b[97;5u"));
    QCOMPARE(terminal.feed("\x1b[<u\x1b[?u"), QByteArray("\x1b[?0u"));
    QCOMPARE(terminal.send({TerminalKeyStroke::character(QStringLiteral("a"), Qt::ControlModifier)}),
             QByteArray("\x01"));
}

void GhosttyEngineTest::win32ModeSendsKeyRecords()
{
    // The worker's byte scanner turns DECSET 9001 on; console programs then
    // read the key they pressed.
    Terminal terminal;
    terminal.engine.setWin32InputMode(true);
    TerminalKeyStroke release = physicalUp();
    release.event = TerminalKeyEvent::Release;
    QCOMPARE(terminal.send({physicalUp(), release}),
             keyRecord(0x26, 0x48, 0, true, 0x100) + keyRecord(0x26, 0x48, 0, false, 0x100));

    // Text no key produced: ASCII keeps its bytes, anything else becomes
    // character records, never UTF-8.
    const QString curly(QChar(0x201c));
    terminal.output.clear();
    terminal.engine.sendText(QStringLiteral("a") + curly);
    terminal.engine.sendPasteBody(QString::fromUcs4(U"\U0001F600", 1));
    QCOMPARE(std::exchange(terminal.output, {}),
             QByteArray("a") + characterRecords(0x201c) + characterRecords(0xd83d) + characterRecords(0xde00));

    // A key with no physical identity keeps its VT meaning.
    QCOMPARE(terminal.send({TerminalKeyStroke::named(TerminalKey::Enter)}), QByteArray("\r"));

    terminal.engine.setWin32InputMode(false);
    QCOMPARE(terminal.send({physicalUp()}), QByteArray("\x1b[A"));
}

void GhosttyEngineTest::negotiatedVtProtocolOverridesWin32Mode()
{
    // A child that negotiates a VT keyboard protocol wants those encodings,
    // which conhost passes through verbatim.
    Terminal terminal;
    terminal.engine.setWin32InputMode(true);
    terminal.feed("\x1b[>1u");
    QCOMPARE(terminal.send({physicalUp(Qt::ControlModifier)}), QByteArray("\x1b[1;5A"));
    terminal.feed("\x1b[<u");
    terminal.engine.setModifyOtherKeys(2);
    QCOMPARE(terminal.send({physicalUp(Qt::ControlModifier)}), QByteArray("\x1b[1;5A"));
    terminal.engine.setModifyOtherKeys(0);
    QCOMPARE(terminal.send({physicalUp(Qt::ControlModifier)}), keyRecord(0x26, 0x48, 0, true, 0x108));
}

void GhosttyEngineTest::wrapsBracketedPaste()
{
    Terminal terminal;
    terminal.engine.beginPaste();
    terminal.engine.sendPasteBody(QStringLiteral("plain"));
    terminal.engine.endPaste();
    QCOMPARE(std::exchange(terminal.output, {}), QByteArray("plain"));

    terminal.feed("\x1b[?2004h");
    terminal.engine.beginPaste();
    terminal.engine.sendPasteBody(QStringLiteral("paste"));
    terminal.engine.endPaste();
    QCOMPARE(std::exchange(terminal.output, {}), QByteArray("\x1b[200~paste\x1b[201~"));
}

void GhosttyEngineTest::forwardsMouseAndFocusReports()
{
    Terminal terminal;
    QVERIFY(!terminal.engine.mouseTracking());
    terminal.engine.sendMouseButton(TerminalMouseButton::Left, true, 1, 2, None);
    QVERIFY(terminal.output.isEmpty());

    terminal.feed("\x1b[?1000h\x1b[?1006h");
    QVERIFY(terminal.engine.mouseTracking());
    terminal.engine.sendMouseButton(TerminalMouseButton::Left, true, 1, 2, None);
    terminal.engine.sendMouseButton(TerminalMouseButton::Left, false, 1, 2, None);
    QCOMPARE(std::exchange(terminal.output, {}), QByteArray("\x1b[<0;3;2M\x1b[<0;3;2m"));
    terminal.engine.sendMouseButton(TerminalMouseButton::WheelLeft, true, 1, 2, None);
    terminal.engine.sendMouseButton(TerminalMouseButton::WheelRight, true, 1, 2, None);
    QCOMPARE(std::exchange(terminal.output, {}), QByteArray("\x1b[<66;3;2M\x1b[<67;3;2M"));

    terminal.engine.setFocused(true);
    QVERIFY(terminal.output.isEmpty());
    terminal.feed("\x1b[?1004h");
    terminal.engine.setFocused(true);
    terminal.engine.setFocused(false);
    QCOMPARE(terminal.output, QByteArray("\x1b[I\x1b[O"));
}

QTEST_MAIN(GhosttyEngineTest)
#include "ghostty_engine_test.moc"
