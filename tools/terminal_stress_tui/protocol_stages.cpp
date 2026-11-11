#include "protocol_stages.h"

#include <algorithm>
#include <array>
#include <sstream>
#include <tuple>

namespace spantui::stresstui::protocol {
namespace {

void appendCursor(std::string &output, int row, int column) {
    output += "\x1b[" + std::to_string(std::max(1, row)) + ';'
        + std::to_string(std::max(1, column)) + 'H';
}

void appendRow(std::string &output, int row, std::string_view text) {
    appendCursor(output, row, 1);
    output += "\x1b[2K";
    output.append(text);
}

std::string repeat(std::string_view text, int count) {
    std::string result;
    result.reserve(text.size() * std::size_t(std::max(0, count)));
    for (int index = 0; index < count; ++index) result.append(text);
    return result;
}

std::tuple<int, int, int> colorWheel(int phase) {
    phase %= 1536;
    if (phase < 0) phase += 1536;
    const int segment = phase / 256;
    const int offset = phase % 256;
    switch (segment) {
    case 0: return {255, offset, 0};
    case 1: return {255 - offset, 255, 0};
    case 2: return {0, 255, offset};
    case 3: return {0, 255 - offset, 255};
    case 4: return {offset, 0, 255};
    default: return {255, 0, 255 - offset};
    }
}

void appendBackground(std::string &output, int red, int green, int blue) {
    output += "\x1b[48;2;" + std::to_string(red) + ';' + std::to_string(green) + ';'
        + std::to_string(blue) + 'm';
}

std::string header(const FrameRequest &request) {
    const auto names = stageNames();
    const std::size_t stage = static_cast<std::size_t>(request.stage);
    std::ostringstream stream;
    stream << "\x1b[1;37;44m SpanTui protocol coverage \x1b[0m  "
           << "STAGE " << (stage + 1) << '/' << names.size() << "  " << names.at(stage)
           << "  " << request.size.columns << 'x' << request.size.rows
           << "  frame " << request.frameIndex;
    return stream.str();
}

void appendFooter(std::string &output, const FrameRequest &request, std::string_view goal) {
    const std::array<std::string_view, 4> spinner{"\u25f0", "\u25f3", "\u25f2", "\u25f1"};
    std::string text = "\x1b[2m";
    text.append(spinner[request.frameIndex % spinner.size()]);
    text += "  observe: ";
    text.append(goal);
    text += "  |  no input required  |  Ctrl+C or close window to stop\x1b[0m";
    appendRow(output, request.size.rows, text);
}

void appendStaticTextStyles(std::string &output, const FrameRequest &request) {
    appendRow(output, 3, "SGR classic     normal  \x1b[1mbold\x1b[22m  \x1b[2mfaint\x1b[22m  "
                         "\x1b[3mitalic\x1b[23m  \x1b[9mstrike\x1b[29m");
    appendRow(output, 5, "Video modes     \x1b[7m reverse \x1b[27m  \x1b[8mhidden\x1b[28m visible  "
                         "\x1b[5mslow blink\x1b[25m  \x1b[6mrapid blink\x1b[25m");
    appendRow(output, 7, "Underline       \x1b[4:1msingle\x1b[4:0m  \x1b[4:2mdouble\x1b[4:0m  "
                         "\x1b[4:3mcurly\x1b[4:0m  \x1b[4:4mdotted\x1b[4:0m  "
                         "\x1b[4:5mdashed\x1b[4:0m");
    appendRow(output, 9, "Rare styles     \x1b[51mframed\x1b[54m  \x1b[52mencircled\x1b[54m  "
                         "\x1b[53moverline\x1b[55m  \x1b[73msuperscript\x1b[75m  \x1b[74msubscript\x1b[75m");
    appendRow(output, 11, "Scripts         Latin 0123  \u4e2d\u6587\u5b57\u5f62  \u65e5\u672c\u8a9e  "
                         "\ud55c\uae00  \u041a\u0438\u0440\u0438\u043b\u043b\u0438\u0446\u0430  \u0639\u0631\u0628\u064a");
    appendRow(output, 13, "Combining       e\u0301  A\u030a  Z\u0351  n\u0303  (marks must stay on base glyphs)");
    appendRow(output, 15, "Emoji / ZWJ     \U0001f600  \U0001f680  \U0001f1e8\U0001f1f3  "
                          "\U0001f469\u200d\U0001f4bb  \U0001f468\u200d\U0001f469\u200d\U0001f467\u200d\U0001f466  \u2764\ufe0f");
    appendRow(output, 17, "Powerline/Nerd  \ue0b0 \ue0b2 \ue0a0 \ue0a1  missing glyphs should fall back predictably");
    appendFooter(output, request, "legacy and extended SGR attributes, reset boundaries, blink timing");
}

void appendColorSystems(std::string &output, const FrameRequest &request) {
    if (request.firstFrame) {
        appendRow(output, 3, "ANSI 16 foreground");
        appendCursor(output, 4, 1);
        for (int color = 0; color < 16; ++color) {
            output += "\x1b[" + std::to_string(color < 8 ? 30 + color : 90 + color - 8)
                + "m" + (color < 10 ? " 0" : " ") + std::to_string(color) + "\x1b[0m ";
        }
        appendRow(output, 6, "ANSI 256 background cube (indices 16..231)");
        for (int row = 0; row < 6 && 7 + row < request.size.rows - 5; ++row) {
            appendCursor(output, 7 + row, 1);
            for (int column = 0; column < 36; ++column) {
                const int color = 16 + row * 36 + column;
                output += "\x1b[48;5;" + std::to_string(color) + "m  ";
            }
            output += "\x1b[0m";
        }
        appendRow(output, std::min(14, request.size.rows - 4), "24-bit truecolor animated gradient");
        appendRow(output, std::min(17, request.size.rows - 2),
                  "Colon RGB form  \x1b[38:2::255:110:80mforeground\x1b[0m  "
                  "\x1b[48:2::30:90:160m background \x1b[0m  "
                  "\x1b[58:2::255:80:180m\x1b[4:3munderline color\x1b[0m");
    }

    const int gradientRow = std::min(15, request.size.rows - 3);
    appendCursor(output, gradientRow, 1);
    const int width = std::max(8, std::min(request.size.columns / 2, 72));
    for (int column = 0; column < width; ++column) {
        const auto [red, green, blue] = colorWheel(column * 1536 / width
                                                   + int(request.frameIndex * 17));
        appendBackground(output, red, green, blue);
        output += "  ";
    }
    output += "\x1b[0m";
    appendFooter(output, request, "palette accuracy, smooth gradients, color-only frame updates");
}

void appendCellGeometry(std::string &output, const FrameRequest &request) {
    if (request.firstFrame) {
        appendRow(output, 3, "Light box       \u250c\u2500\u2500\u2500\u252c\u2500\u2500\u2500\u2510   rounded  \u256d\u2500\u2500\u256e");
        appendRow(output, 4, "                \u2502   \u2502   \u2502            \u2502  \u2502");
        appendRow(output, 5, "                \u251c\u2500\u2500\u2500\u253c\u2500\u2500\u2500\u2524            \u2570\u2500\u2500\u256f");
        appendRow(output, 7, "Heavy/double    \u250f\u2501\u2501\u2533\u2501\u2501\u2513   \u2554\u2550\u2566\u2550\u2557   mixed \u250d\u2501\u2511 \u2552\u2550\u2555");
        appendRow(output, 9, "Blocks          \u2588 \u2587 \u2586 \u2585 \u2584 \u2583 \u2582 \u2581   \u258f\u258e\u258d\u258c\u258b\u258a\u2589\u2588   \u2591\u2592\u2593");
        appendRow(output, 11, "Quadrants       \u2596\u2597 \u2598\u259d \u259a \u259e \u2599 \u259f \u259b \u259c");
        appendRow(output, 13, "Braille lattice (animated diagonal must remain continuous across cells/rows)");
    }
    for (int row = 0; row < 4 && 14 + row < request.size.rows; ++row) {
        appendCursor(output, 14 + row, 1);
        for (int column = 0; column < std::min(64, request.size.columns); ++column) {
            const int phase = (column * 2 + row * 3 + int(request.frameIndex)) % 8;
            const char32_t glyph = char32_t(0x2800 + (1 << phase));
            if (glyph < 0x800) {
                output.push_back(char(0xc0 | (glyph >> 6)));
                output.push_back(char(0x80 | (glyph & 0x3f)));
            } else {
                output.push_back(char(0xe0 | (glyph >> 12)));
                output.push_back(char(0x80 | ((glyph >> 6) & 0x3f)));
                output.push_back(char(0x80 | (glyph & 0x3f)));
            }
        }
    }
    appendFooter(output, request, "joined strokes, pixel snapping, block coverage, braille pitch");
}

void appendWidthAndWrapping(std::string &output, const FrameRequest &request) {
    if (request.firstFrame) {
        appendRow(output, 3, "Column ruler");
        appendCursor(output, 4, 1);
        for (int column = 1; column <= request.size.columns; ++column)
            output.push_back(char('0' + (column % 10)));
        appendRow(output, 6, "Narrow cells : |abcdefghijklmnopqrstuvwxyz|");
        appendRow(output, 8, "Wide CJK     : |\u4e00\u4e8c\u4e09\u56db\u4e94\u516d\u4e03\u516b\u4e5d\u5341| <- right bar should align after 20 cells");
        appendRow(output, 10, "Combining    : |e\u0301e\u0301e\u0301e\u0301e\u0301| <- five visible cells");
        appendRow(output, 12, "Ambiguous    : |\u00b7\u00d7\u00f7\u03a9\u03b1\u03b2| width policy must be stable");
        appendRow(output, 14, "Boundary wrap: final printable cell and following glyph must wrap exactly once");
        if (request.size.columns >= 12 && request.size.rows >= 18) {
            appendCursor(output, 16, request.size.columns - 7);
            output += "[EDGE]>XY";
        }
    }
    appendFooter(output, request, "cell widths, right-edge wrapping, resize reflow, stable alignment");
}

void appendScrolling(std::string &output, const FrameRequest &request) {
    const int top = 4;
    const int bottom = std::max(top + 1, request.size.rows - 3);
    if (request.firstFrame) {
        appendRow(output, 2, "DECSTBM/SU/SD/IND/RI: only middle region scrolls; outer rows stay fixed.");
        output += "\x1b[" + std::to_string(top) + ';' + std::to_string(bottom) + 'r';
        for (int row = top; row <= bottom; ++row) {
            appendCursor(output, row, 1);
            output += "| initial row " + std::to_string(row - top) + ' '
                + repeat("-", std::max(0, request.size.columns - 20)) + '|';
        }
        appendCursor(output, top, 1);
        output += "\x1bM"; // RI at top scrolls the region down.
        appendCursor(output, bottom, 1);
        output += "\x1b" "D"; // IND at bottom scrolls the region up.
        output += "\x1b[S\x1b[T"; // SU then SD.
    }
    appendCursor(output, bottom, 1);
    output += "\r\n\x1b[38;5;" + std::to_string(16 + request.frameIndex % 216)
        + "m| scrolled frame " + std::to_string(request.frameIndex) + "\x1b[0m";
    appendFooter(output, request, "DECSTBM scroll region, line insertion, stable rows outside region");
}

void appendLinksAndDecorations(std::string &output, const FrameRequest &request) {
    if (request.firstFrame) {
        output += "\x1b]0;SpanTui Protocol — OSC 0\x1b\\"
                  "\x1b]2;SpanTui Protocol — OSC 2\x1b\\";
        appendRow(output, 3, "OSC 8 hyperlink  \x1b]8;id=stress-tui;https://example.com/spantui\x1b\\"
                         "https://example.com/spantui\x1b]8;;\x1b\\  <- should be link-styled/hittable");
        appendRow(output, 6, "Underline color \x1b[58;2;255;80;120m\x1b[4:3mcurly pink\x1b[4:0;59m  "
                         "\x1b[58;2;80;190;255m\x1b[4:2mdouble blue\x1b[4:0;59m");
        appendRow(output, 9, "Decorations     \x1b[4munderline\x1b[24m  \x1b[9mstrike\x1b[29m  "
                         "\x1b[53moverline\x1b[55m  \x1b[7mreverse\x1b[27m");
        appendRow(output, 12, "OSC title       window title should read: SpanTui Protocol");
    }
    appendFooter(output, request, "OSC 8 identity, underline variants/colors, title handling");
}

std::string base64Encode(std::string_view bytes) {
    static constexpr std::string_view alphabet =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string result;
    result.reserve((bytes.size() + 2) / 3 * 4);
    for (std::size_t offset = 0; offset < bytes.size(); offset += 3) {
        const unsigned first = static_cast<unsigned char>(bytes[offset]);
        const unsigned second = offset + 1 < bytes.size()
            ? static_cast<unsigned char>(bytes[offset + 1]) : 0;
        const unsigned third = offset + 2 < bytes.size()
            ? static_cast<unsigned char>(bytes[offset + 2]) : 0;
        const unsigned value = (first << 16) | (second << 8) | third;
        result.push_back(alphabet[(value >> 18) & 63]);
        result.push_back(alphabet[(value >> 12) & 63]);
        result.push_back(offset + 1 < bytes.size() ? alphabet[(value >> 6) & 63] : '=');
        result.push_back(offset + 2 < bytes.size() ? alphabet[value & 63] : '=');
    }
    return result;
}

void appendInlineGraphics(std::string &output, const FrameRequest &request) {
    if (request.firstFrame) {
        appendRow(output, 2, "Three protocol images should appear below at their requested cell positions.");

        std::string rgba;
        rgba.reserve(16 * 16 * 4);
        for (int y = 0; y < 16; ++y) {
            for (int x = 0; x < 16; ++x) {
                const bool accent = ((x / 4) + (y / 4)) % 2 == 0;
                rgba.push_back(char(accent ? 236 : 35));
                rgba.push_back(char(accent ? 72 : 180));
                rgba.push_back(char(accent ? 92 : 220));
                rgba.push_back(char(255));
            }
        }
        appendCursor(output, 5, 3);
        output += "Kitty raw RGBA";
        appendCursor(output, 6, 3);
        output += "\x1b_Ga=T,f=32,s=16,v=16,c=8,r=5,i=7001;" + base64Encode(rgba) + "\x1b\\";

        // 1x1 red PNG. The requested cell extent makes placement boundaries visible.
        constexpr std::string_view redPng =
            "iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAQAAAC1HAwCAAAAC0lEQVR42mP8/x8AAusB9Wl2nWQAAAAASUVORK5CYII=";
        appendCursor(output, 5, 24);
        output += "iTerm2 PNG";
        appendCursor(output, 6, 24);
        output += "\x1b]1337;File=inline=1;width=8;height=5:";
        output.append(redPng);
        output += "\x1b\\";

        appendCursor(output, 5, 45);
        output += "Sixel";
        appendCursor(output, 6, 45);
        output += "\x1bPq#1;2;100;20;30#1!24~$!24~$!24~$!24~-$!24~$!24~$!24~$!24~\x1b\\";
    }
    appendFooter(output, request, "Kitty, iTerm2 and Sixel decode/placement/cleanup");
}

void appendLegacyCharsets(std::string &output, const FrameRequest &request) {
    if (request.frameIndex == 0) {
        // DECALN is intentionally given one frame on its own: every cell must
        // become E before the next frame restores the stage explanation.
        output += "\x1b#8";
        return;
    }
    if (request.frameIndex < 12 && !request.firstFrame) return;
    if (request.frameIndex == 12 || request.firstFrame) {
        output += "\x1b[2J\x1b[H";
        appendRow(output, 1, header(request));
        appendRow(output, 3, "DEC Special Graphics (G0):");
        appendCursor(output, 4, 3);
        output += "\x1b(0lqqqqk\x1b(B  l/q/k must render as a joined top border";
        appendCursor(output, 5, 3);
        output += "\x1b(0x    x\x1b(B  x must render as vertical strokes";
        appendCursor(output, 6, 3);
        output += "\x1b(0mqqqqj\x1b(B  m/q/j must render as a joined bottom border";
        appendRow(output, 8, "C0 controls: A<TAB>B, overwrite via BS, and CR return");
        appendCursor(output, 9, 3);
        output += "A\tB   12\bX   tail\rCR";
        appendRow(output, 11, "DECSC/DECRC: save cursor, draw elsewhere, restore, continue");
        appendCursor(output, 12, 3);
        output += "A\x1b" "7\x1b[12;30HB\x1b" "8C";
        appendRow(output, 14, "VT52 compatibility: DECANM reset, direct address, then ESC < returns ANSI");
        output += "\x1b[?2l\x1bY0*VT52@16,11\x1b<";
        appendRow(output, 17, "DEC line attributes: normal / double-width / double-height top+bottom");
        appendCursor(output, 18, 3);
        output += "\x1b#5normal width\x1b[19;3H\x1b#6double width\x1b[20;3H\x1b#3DOUBLE HEIGHT"
                  "\x1b[21;3H\x1b#4DOUBLE HEIGHT\x1b#5";
    }
    appendFooter(output, request, "DECALN, C0, G0 charset, VT52, save/restore, line attributes");
}

void appendCursorAndEditing(std::string &output, const FrameRequest &request) {
    if (request.firstFrame) {
        appendRow(output, 3, "Cursor movement: CUP/HVP, CUU/CUD/CUF/CUB, CHA/VPA");
        appendRow(output, 5, "0123456789  <- ICH inserts 3 blanks before 5");
        appendCursor(output, 5, 6);
        output += "\x1b[3@";
        appendRow(output, 7, "0123456789  <- DCH removes three cells at column 4");
        appendCursor(output, 7, 4);
        output += "\x1b[3P";
        appendRow(output, 9, "0123456789  <- ECH erases four cells without shifting");
        appendCursor(output, 9, 5);
        output += "\x1b[4X";
        appendRow(output, 11, "EL0 clears right | EL1 clears left | EL2 clears whole row");
        appendCursor(output, 12, 20);
        output += "left survives\x1b[K";
        appendRow(output, 14, "REP: Z followed by CSI 12 b must produce thirteen Z glyphs");
        appendCursor(output, 15, 3);
        output += "Z\x1b[12b";
        appendRow(output, 17, "IL/DL line editing region (labels must remain ordered)");
        appendRow(output, 18, "line A");
        appendRow(output, 19, "line B");
        appendRow(output, 20, "line C");
        appendCursor(output, 19, 1);
        output += "\x1b[Linserted line\x1b[20;1H\x1b[M";
    }
    const int row = std::min(request.size.rows - 2, 23);
    const int span = std::max(1, request.size.columns - 12);
    appendRow(output, row, "");
    appendCursor(output, row, 1 + int(request.frameIndex % std::uint64_t(span)));
    output += "\x1b[1;33mCURSOR\x1b[0m";
    appendFooter(output, request, "cursor addressing, erase/insert/delete/REP semantics");
}

void appendModernModes(std::string &output, const FrameRequest &request) {
    if (request.firstFrame) {
        appendRow(output, 3, "DEC/private modes are enabled and restored automatically in this stage:");
        appendRow(output, 5, "  ?1 application cursor   ?6 origin   ?7 autowrap   ?25 cursor visibility");
        appendRow(output, 7, "  ?1000/?1006 mouse       ?1004 focus       ?2004 bracketed paste");
        appendRow(output, 9, "  Kitty keyboard push/pop (>3u / <u) and xterm modifyOtherKeys");
        appendRow(output, 11, "DECSCUSR cursor shapes rotate below: block, underline, bar; blinking/steady");
        output += "\x1b[?1h\x1b[?1l\x1b[?6h\x1b[?6l\x1b[?7l\x1b[?7h"
                  "\x1b[?1000h\x1b[?1006h\x1b[?1006l\x1b[?1000l"
                  "\x1b[?1004h\x1b[?1004l\x1b[?2004h\x1b[?2004l"
                  "\x1b[>3u\x1b[<u\x1b[>4;2m\x1b[>4;0m";
    }
    static constexpr std::array<int, 6> shapes{1, 2, 3, 4, 5, 6};
    const int shape = shapes[(request.frameIndex / 8) % shapes.size()];
    appendRow(output, 14, "active cursor shape code: " + std::to_string(shape));
    appendFooter(output, request, "mode transitions restore cleanly; cursor shape/visibility is stable");
    appendCursor(output, 16, 8 + int(request.frameIndex % 24));
    output += "\x1b[?25h\x1b[" + std::to_string(shape) + " q";
}

} // namespace

std::vector<std::string_view> stageNames() {
    return {
        "legacy-charsets",
        "cursor-and-editing",
        "margins-and-scrolling",
        "legacy-sgr",
        "extended-color",
        "unicode-width",
        "cell-geometry",
        "modern-osc",
        "modern-modes",
        "inline-graphics",
    };
}

std::string renderFrame(const FrameRequest &request) {
    FrameRequest safe = request;
    safe.size.columns = std::clamp(safe.size.columns, 20, 1000);
    safe.size.rows = std::clamp(safe.size.rows, 8, 300);

    std::string output;
    if (safe.synchronizedUpdates) output += "\x1b[?2026h";
    if (safe.firstFrame) {
        output += "\x1b[0m\x1b[r\x1b[?6l\x1b[?7h\x1b[?69l"
                  "\x1b[?1000l\x1b[?1004l\x1b[?1006l\x1b[?2004l\x1b[<u"
                  "\x1b[0 q\x1b[19;1H\x1b#5\x1b[20;1H\x1b#5\x1b[21;1H\x1b#5"
                  "\x1b]8;;\x1b\\\x1b_Ga=d;\x1b\\\x1b[?25l\x1b[2J\x1b[H";
    }
    appendRow(output, 1, header(safe));

    switch (safe.stage) {
    case Stage::LegacyCharsets: appendLegacyCharsets(output, safe); break;
    case Stage::CursorAndEditing: appendCursorAndEditing(output, safe); break;
    case Stage::MarginsAndScrolling: appendScrolling(output, safe); break;
    case Stage::LegacySgr: appendStaticTextStyles(output, safe); break;
    case Stage::ExtendedColor: appendColorSystems(output, safe); break;
    case Stage::UnicodeWidth: appendWidthAndWrapping(output, safe); break;
    case Stage::CellGeometry: appendCellGeometry(output, safe); break;
    case Stage::ModernOsc: appendLinksAndDecorations(output, safe); break;
    case Stage::ModernModes: appendModernModes(output, safe); break;
    case Stage::InlineGraphics: appendInlineGraphics(output, safe); break;
    case Stage::Count: break;
    }

    if (safe.synchronizedUpdates) output += "\x1b[?2026l";
    return output;
}

std::string resetModes() {
    return "\x1b[?2026l\x1b[r\x1b[?6l\x1b[?7h\x1b[?69l"
           "\x1b[?1000l\x1b[?1004l\x1b[?1006l\x1b[?2004l\x1b[<u"
           "\x1b[0 q\x1b]8;;\x1b\\\x1b_Ga=d;\x1b\\\x1b[0m\x1b[?25h\x1b]2;\x1b\\";
}

} // namespace spantui::stresstui::protocol
