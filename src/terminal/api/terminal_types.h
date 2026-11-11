#pragma once

#include <QColor>
#include <QHashFunctions>
#include <QPoint>
#include <QSize>
#include <QString>
#include <QVector>
#include <QImage>
#include <QMetaType>
#include <memory>
#include <optional>

namespace span::terminal {

enum class TerminalKey {
    Enter,
    Tab,
    Backspace,
    Escape,
    Up,
    Down,
    Left,
    Right,
    Insert,
    Delete,
    Home,
    End,
    PageUp,
    PageDown,
    Function
};

enum class TerminalKeyEvent : quint8 { Press, Repeat, Release };

// Logical identity accompanying text from QKeyEvent. Kitty flag 4 uses these
// optional alternate values so shortcuts keep working across Shift and
// non-Latin keyboard layouts. IME-only commits legitimately leave them zero.
struct TerminalTextKey {
    int codepoint = 0;
    int shiftedCodepoint = 0;
    int baseLayoutCodepoint = 0;
};

// KEY_EVENT_RECORD dwControlKeyState bits, as win32-input-mode carries them.
namespace TerminalControlKeyState {
constexpr quint32 RightAlt = 0x0001;
constexpr quint32 LeftAlt = 0x0002;
constexpr quint32 RightCtrl = 0x0004;
constexpr quint32 LeftCtrl = 0x0008;
constexpr quint32 Shift = 0x0010;
constexpr quint32 NumLock = 0x0020;
constexpr quint32 ScrollLock = 0x0040;
constexpr quint32 CapsLock = 0x0080;
constexpr quint32 EnhancedKey = 0x0100;
}

// The keyboard's own description of a key event: what a Windows console
// program would read from ReadConsoleInput. virtualKey 0 means the event did
// not come from a physical key (synthetic events, IME placeholders).
struct TerminalNativeKey {
    quint16 virtualKey = 0;
    quint16 scanCode = 0;
    quint32 controlKeyState = 0;
    // What the layout produced, unshaped: Ctrl+A is U+0001, Enter is CR.
    QString text;
};

// One key event. A named key (`key`) or a character key (`text` plus the
// `textKey` identity) is what VT keyboard protocols encode; `native` is what
// conhost needs to hand a console program the key it actually pressed.
struct TerminalKeyStroke {
    TerminalKeyEvent event = TerminalKeyEvent::Press;
    // Protocol modifiers (Shift, Ctrl, Alt, Meta); AltGr is already folded
    // into the produced text.
    Qt::KeyboardModifiers modifiers = Qt::NoModifier;
    std::optional<TerminalKey> key;
    int functionNumber = 0;
    QString text;
    TerminalTextKey textKey;
    // Numeric keypad digits, operators and Enter: Kitty's report-all-keys
    // gives them their own KP_* codes instead of the main keys'.
    bool keypad = false;
    TerminalNativeKey native;

    static TerminalKeyStroke named(TerminalKey key,
                                   Qt::KeyboardModifiers modifiers = Qt::NoModifier,
                                   int functionNumber = 0,
                                   TerminalKeyEvent event = TerminalKeyEvent::Press) {
        TerminalKeyStroke stroke;
        stroke.event = event;
        stroke.modifiers = modifiers;
        stroke.key = key;
        stroke.functionNumber = functionNumber;
        return stroke;
    }
    static TerminalKeyStroke character(const QString &text,
                                       Qt::KeyboardModifiers modifiers = Qt::NoModifier,
                                       TerminalKeyEvent event = TerminalKeyEvent::Press,
                                       TerminalTextKey textKey = {}) {
        TerminalKeyStroke stroke;
        stroke.event = event;
        stroke.modifiers = modifiers;
        stroke.text = text;
        stroke.textKey = textKey;
        return stroke;
    }
};

// As drawn. The user's setting is the engine's default shape, which the
// child may override with DECSCUSR and restore with DECSCUSR 0.
enum class CursorShape {
    Block,
    Underline,
    Bar
};

// SGR 4:n.
enum class UnderlineStyle : quint8 {
    None,
    Single,
    Double,
    Curly,
    Dotted,
    Dashed
};

enum class TerminalMouseButton : quint8 {
    Left = 1,
    Middle = 2,
    Right = 3,
    WheelUp = 4,
    WheelDown = 5,
    WheelLeft = 6,
    WheelRight = 7
};

struct CellStyle {
    QColor foreground;
    QColor background;
    // ANSI 0-15 resolve against the theme at paint; 256-colour and RGB
    // values stay in foreground/background.
    int foregroundPaletteIndex = -1;
    int backgroundPaletteIndex = -1;
    bool defaultForeground = true;
    bool defaultBackground = true;
    bool bold = false;
    // SGR 2. Resolved at paint by blending the ink toward the cell background.
    bool faint = false;
    bool italic = false;
    UnderlineStyle underline = UnderlineStyle::None;
    // SGR 58. Neither set means the underline follows the text colour; ANSI
    // 0-15 stay palette slots so a theme switch recolours them like text.
    QColor underlineColor;
    int underlineColorPaletteIndex = -1;
    bool overline = false;
    bool strike = false;
    bool blink = false;
    // SGR 7 stays an attribute until paint: swapping unresolved defaults is
    // a no-op and leaves default-colour reverse cells invisible.
    bool reverse = false;
    // SGR 8 stays an attribute until paint: baking it into fg = bg loses the
    // palette slot and semantic default, and contrast repair re-inks it.
    bool conceal = false;
    QString hyperlink;

    bool operator==(const CellStyle &other) const
    {
        return foreground == other.foreground && background == other.background
            && foregroundPaletteIndex == other.foregroundPaletteIndex
            && backgroundPaletteIndex == other.backgroundPaletteIndex && defaultForeground == other.defaultForeground
            && defaultBackground == other.defaultBackground && bold == other.bold && faint == other.faint
            && italic == other.italic && underline == other.underline && underlineColor == other.underlineColor
            && underlineColorPaletteIndex == other.underlineColorPaletteIndex && overline == other.overline
            && strike == other.strike && blink == other.blink && reverse == other.reverse && conceal == other.conceal
            && hyperlink == other.hyperlink;
    }
    bool operator!=(const CellStyle &other) const { return !(*this == other); }
};

struct Cell {
    QString text;
    CellStyle style;
    int width = 1;
    bool continuation = false;

    bool operator==(const Cell &other) const
    {
        return text == other.text && width == other.width && continuation == other.continuation && style == other.style;
    }
    bool operator!=(const Cell &other) const { return !(*this == other); }
};

// A row's identity across frames (libghostty's render-state row id): it
// follows the row as output scrolls it into history and while the viewport
// moves, and is never reused. Null means no row.
struct TerminalRowId {
    quint64 high = 0;
    quint64 low = 0;

    bool isNull() const { return high == 0 && low == 0; }
    bool operator==(const TerminalRowId &other) const { return high == other.high && low == other.low; }
    bool operator!=(const TerminalRowId &other) const { return !(*this == other); }
};

inline size_t qHash(const TerminalRowId &id, size_t seed = 0) noexcept
{
    return qHashMulti(seed, id.high, id.low);
}

struct Line {
    TerminalRowId id;
    // Changes whenever the row's cells do; with `id` it names the content,
    // so a prepared row can be reused wherever the row moved to.
    quint64 revision = 0;
    QVector<Cell> cells;
    bool softWrapped = false;
    // Cells in use before a soft wrap; -1 means all of them. A wide glyph
    // that does not fit wraps early and leaves the rest of the row as
    // structural padding, not text, until something is written there.
    int wrapColumns = -1;

    int usedColumns() const
    {
        const int columns = int(cells.size());
        return softWrapped && wrapColumns >= 0 ? qMin(wrapColumns, columns) : columns;
    }
};

// Decoded pixels shared by every placement of one image version.
struct GraphicResource {
    QString id;
    QImage image;
    qint64 byteSize() const { return image.sizeInBytes(); }
};

// A Kitty graphics placement as the viewport shows it.
struct GraphicPlacement {
    QString id;
    std::shared_ptr<const GraphicResource> resource;
    // Top-left cell relative to the viewport's first row; negative while the
    // image starts above it.
    int row = 0;
    int column = 0;
    // Size in cells, and the pixel offset of the image inside its first cell.
    int columns = 0;
    int rows = 0;
    QPoint cellOffset;
    // Drawn size in pixels of the image's own resolution (c/r scale it).
    QSize pixelSize;
    int zIndex = 0;
};

struct Cursor {
    int row = 0;
    int column = 0;
    bool visible = true;
    bool blink = true;
    CursorShape shape = CursorShape::Block;
    // OSC 12; invalid means the theme's cursor colour.
    QColor color;
};

// Where the viewport is in the scrollable rows (history plus the active
// screen), in rows from the oldest one.
struct TerminalScrollbar {
    qint64 total = 0;
    qint64 offset = 0;
    int rows = 0;

    bool atBottom() const { return offset + rows >= total; }
};

// Columns [startColumn, endColumn) of the frame line `line` (an index into
// Frame::lines) drawn highlighted.
struct TerminalHighlight {
    int line = 0;
    int startColumn = 0;
    int endColumn = 0;
    // The selected search match.
    bool current = false;

    bool operator==(const TerminalHighlight &other) const
    {
        return line == other.line && startColumn == other.startColumn && endColumn == other.endColumn
            && current == other.current;
    }
};

// The viewport and the rows around it. `lines` holds up to a screen of
// overscan above and below the viewport, so the view can draw a scroll
// position between two frames without waiting for the core.
struct Frame {
    int columns = 0;
    int rows = 0;
    QVector<Line> lines;
    // Index in `lines` of the viewport's first row.
    int viewportRow = 0;
    TerminalScrollbar scrollbar;
    // In viewport coordinates, captured atomically with the cells. Not
    // visible while the viewport is scrolled away from it.
    Cursor cursor;
    // The alternate screen has no history to scroll; the view turns the
    // wheel into cursor keys there instead (alternate scroll mode).
    bool alternateScreen = false;
    // The shell marks its prompts (OSC 133), so prompt jumps have targets.
    bool promptMarks = false;
    bool hasSelection = false;
    QVector<TerminalHighlight> selection;
    QVector<TerminalHighlight> searchMatches;
    QVector<GraphicPlacement> graphics;
};

// Find over history and screen (libghostty search: literal text, ASCII
// letters case-insensitive). `selected` is 0-based, -1 when no match is
// selected; `complete` is false while matches are still being counted.
struct TerminalSearchStatus {
    QString needle;
    int total = 0;
    int selected = -1;
    bool complete = true;
};

// ConEmu / Windows Terminal progress (OSC 9;4), as shown on the taskbar.
enum class TerminalProgressState : quint8 { None, Normal, Error, Indeterminate, Paused };

// Out-of-band requests a child makes of its host: BEL, desktop notifications
// (OSC 9, OSC 777) and progress (OSC 9;4). They carry no screen state and
// never enter a frame.
struct TerminalHostEvent {
    enum class Kind : quint8 { Bell, Notification, Progress };
    Kind kind = Kind::Bell;
    QString title;
    QString text;
    TerminalProgressState progress = TerminalProgressState::None;
    int progressValue = 0;
};

// A renderable terminal state published by the worker thread. The Frame is
// immutable after publication; rows are reused by (id, revision), and
// sequence/fullRefresh make skipped coalesced frames safe to consume.
struct TerminalFrameSnapshot {
    Frame frame;
    TerminalSearchStatus search;
    quint64 sequence = 0;
    quint64 graphicsRevision = 0;
    quint64 transportEpoch = 0;
    quint64 viewportEpoch = 0;
    // The last view scroll request (TerminalSession::scrollViewport) the
    // viewport reflects.
    quint64 scrollRequest = 0;
    // Changes when the core moved the viewport on its own account (a search
    // or prompt jump, selection autoscroll, a resize); the view then takes
    // the frame's position instead of its own.
    quint64 viewportJump = 0;
    int mouseMode = 0;
    bool synchronizedUpdateActive = false;
    bool fullRefresh = true;
};

// A cell of the drawn grid, row 0 at its top.
struct TerminalViewportCell {
    int row = 0;
    int column = 0;
};

// One pointer event of a selection gesture. `row` is relative to the frame's
// viewport (negative above it, in overscan) of the frame whose scrollbar
// offset is `offset`; `x`/`y` are surface pixels. Clicks closer together
// than `repeatIntervalMs` extend to words, then lines.
struct TerminalSelectionEvent {
    enum class Kind : quint8 { Press, Drag, Release };
    Kind kind = Kind::Press;
    int row = 0;
    int column = 0;
    qint64 offset = 0;
    qreal x = 0;
    qreal y = 0;
    qint64 timeMs = 0;
    int repeatIntervalMs = 400;
    bool rectangle = false;
};

} // namespace span::terminal

Q_DECLARE_METATYPE(span::terminal::TerminalHostEvent)
Q_DECLARE_METATYPE(span::terminal::TerminalSelectionEvent)
