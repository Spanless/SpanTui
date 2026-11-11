#pragma once

#include "terminal/api/terminal_types.h"
#include "terminal/terminal_constants.h"

// Qt defines `emit` as an empty macro; libghostty names a struct field so.
#pragma push_macro("emit")
#undef emit
#include <ghostty/vt.h>
#pragma pop_macro("emit")

#include <functional>
#include <memory>
#include <optional>
#include <QByteArray>
#include <QByteArrayView>
#include <QHash>
#include <QSize>
#include <QVector>
#include <Qt>

namespace span::terminal {

// The terminal emulator: libghostty-vt's parser, screens, scrollback,
// selection and search, wrapped for the core worker. It owns everything the
// child process can change; the worker decides when to publish frame().
// One instance serves one transport: a new child gets a new engine.
class GhosttyEngine final {
public:
    using OutputCallback = std::function<void(const QByteArray &)>;
    using TitleCallback = std::function<void(const QString &)>;
    using ClipboardCallback = std::function<void(const QString &)>;
    using HostEventCallback = std::function<void(const TerminalHostEvent &)>;

    explicit GhosttyEngine(int columns = kDefaultColumns, int rows = kDefaultRows);
    ~GhosttyEngine();
    GhosttyEngine(const GhosttyEngine &) = delete;
    GhosttyEngine &operator=(const GhosttyEngine &) = delete;

    // False when libghostty could not allocate the terminal; nothing else
    // may be called then.
    bool isReady() const { return m_ready; }

    // Bytes for the child: device reports and encoded input.
    void setOutputCallback(OutputCallback callback) { m_onOutput = std::move(callback); }
    void setTitleCallback(TitleCallback callback) { m_onTitle = std::move(callback); }
    // OSC 52 writes only. Reads are never answered: the clipboard may hold
    // secrets.
    void setClipboardCallback(ClipboardCallback callback) { m_onClipboard = std::move(callback); }
    void setHostEventCallback(HostEventCallback callback) { m_onHostEvent = std::move(callback); }
    // Rendered cell size in logical pixels: the unit of Kitty image
    // placements, selection gestures and XTWINOPS size reports.
    void setCellPixelSize(QSize size);
    // Theme defaults. The child may override them (OSC 10/11/4); cells keep
    // theme colours semantic (default, ANSI 0-15) until it does.
    void setDefaultColors(const QColor &foreground, const QColor &background);
    void setAnsiPalette(const QVector<QColor> &palette);
    // The user's cursor shape: in force until the child asks for another
    // (DECSCUSR), and again after it resets with DECSCUSR 0.
    void setDefaultCursorShape(CursorShape shape);
    // History rows kept; negative lifts the row limit.
    void setMaxScrollback(int lines);

    void feed(QByteArrayView bytes);
    void resize(int columns, int rows);
    // The user's Clear command: erases the history and every row but the
    // cursor's, which moves to the top (as ConPTY's clear does).
    void clearDisplay();

    // DECSET 9001 and XTMODKEYS 4 as the worker's byte scanner sees them.
    // libghostty does not know win32-input-mode; see TerminalWin32Input.
    void setWin32InputMode(bool enabled) { m_win32InputMode = enabled; }
    void setModifyOtherKeys(int level) { m_modifyOtherKeys = level; }

    // One key event, encoded for whichever keyboard channel is in force.
    void sendKeyStroke(const TerminalKeyStroke &stroke);
    // Text no key produced (IME commit, programmatic input).
    void sendText(const QString &text);
    // Bracketed paste markers when the child enabled DECSET 2004.
    void beginPaste();
    void endPaste();
    // Paste body: encoded as text, never as key events.
    void sendPasteBody(const QString &text);
    bool mouseTracking() const;
    // `row`/`column` are viewport cells.
    void sendMouseMove(int row, int column, Qt::KeyboardModifiers modifiers);
    void sendMouseButton(TerminalMouseButton button, bool pressed, int row, int column,
                         Qt::KeyboardModifiers modifiers);
    void setFocused(bool focused);

    // The viewport's first row, in rows from the oldest history row.
    void scrollViewportTo(qint64 row);
    void scrollViewportToBottom();
    // Moves the viewport top by `delta` OSC 133 prompts.
    void scrollViewportByPrompts(int delta);

    // Applies a pointer event to the selection gesture. Returns true while
    // a drag past the top or bottom edge asks for autoscroll ticks.
    bool selectionEvent(const TerminalSelectionEvent &event);
    // Scrolls one row and extends the selection; false once the gesture
    // stopped autoscrolling.
    bool selectionAutoscrollTick();
    void selectAll();
    void clearSelection();
    // Plain text, soft wraps joined, trailing blanks trimmed.
    QString selectedText() const;

    // An empty needle ends the search.
    void setSearchNeedle(const QString &needle);
    // Older match (toward history) unless `newer`; scrolls it into view.
    void selectSearchMatch(bool newer);
    // Catches the search up with the terminal; true while more work is left
    // than one bounded step does.
    bool advanceSearch();
    TerminalSearchStatus searchStatus() const;

    // The viewport with overscan each side; unchanged rows keep their revision.
    Frame frame();
    // Overscan per side: a screen (default), for smooth scrolling between
    // frames; or one row while the viewport jumps more than a screen per
    // frame, when a screen of overscan would be prepared but never drawn.
    void setWideOverscan(bool wide);
    // Changes whenever the Kitty placements or their pixels do.
    quint64 graphicsRevision() const { return m_graphicsRevision; }
    TerminalScrollbar scrollbar() const;
    qint64 scrollbackRows() const;
    // Compresses idle history pages; `full` also the recent ones.
    void compressHistory(bool full);

private:
    struct CachedLine {
        Line line;
        bool seen = false;
    };

    static void writePtyCallback(GhosttyTerminal, void *user, const uint8_t *data, size_t length);
    static void bellCallback(GhosttyTerminal, void *user);
    static void titleChangedCallback(GhosttyTerminal, void *user);
    static bool sizeCallback(GhosttyTerminal, void *user, GhosttySizeReportSize *size);
    static bool colorSchemeCallback(GhosttyTerminal, void *user, GhosttyColorScheme *scheme);
    static void clipboardWriteCallback(GhosttyTerminal, void *user, const GhosttyClipboardWrite *write);
    static void notificationCallback(GhosttyTerminal, void *user,
                                     const GhosttyTerminalDesktopNotification *notification);
    static void progressCallback(GhosttyTerminal, void *user, const GhosttyTerminalProgressReport *report);
    static void semanticPromptCallback(GhosttyTerminal, void *user, const GhosttyTerminalSemanticPrompt *event);
    static GhosttyString xtversionCallback(GhosttyTerminal, void *user);

    void applyColors();
    void applyOverscan();
    // The palette colour libghostty starts from: the theme's, else its own.
    GhosttyColorRgb basePaletteColor(int index) const;
    void respond(const QByteArray &bytes);
    void emitHostEvent(const TerminalHostEvent &event);
    bool modeEnabled(GhosttyMode mode) const;
    bool darkSurface() const;
    void reportColorScheme();
    bool win32Channel() const;
    QSize cellSize() const;
    QByteArray encodeKey(const TerminalKeyStroke &stroke);
    void encodeMouse(GhosttyMouseAction action, std::optional<GhosttyMouseButton> button, int row, int column,
                     Qt::KeyboardModifiers modifiers);
    void installSelection(const GhosttySelection *selection);
    GhosttySelectionGestureGeometry gestureGeometry() const;
    std::optional<GhosttyGridRef> screenRef(qint64 screenRow, int column) const;
    std::optional<GhosttyPointCoordinate> screenPoint(const GhosttyGridRef &ref) const;
    void readLine(qint64 screenRow, const GhosttyRenderStateColors &colors, Line *line) const;
    void readCell(qint64 screenRow, int column, const GhosttyRenderStateColors &colors, Cell *cell) const;
    void convertStyle(const GhosttyStyle &style, const GhosttyRenderStateColors &colors, CellStyle *cellStyle) const;
    bool resolveColor(const GhosttyStyleColor &color, const GhosttyRenderStateColors &colors, QColor *resolved,
                      int *paletteIndex) const;
    void readSearchMatches(Frame &frame, qint64 firstScreenRow) const;
    void readGraphics(Frame &frame);
    void readCursor(Frame &frame) const;

    GhosttyTerminal m_terminal = nullptr;
    GhosttyRenderState m_renderState = nullptr;
    GhosttyRenderStateRowIterator m_rowIterator = nullptr;
    GhosttyRenderStateRowCells m_cells = nullptr;
    GhosttyRenderStateImageIterator m_images = nullptr;
    GhosttyKeyEncoder m_keyEncoder = nullptr;
    GhosttyKeyEvent m_keyEvent = nullptr;
    GhosttyMouseEncoder m_mouseEncoder = nullptr;
    GhosttyMouseEvent m_mouseEvent = nullptr;
    GhosttySelectionGesture m_gesture = nullptr;
    GhosttySelectionGestureEvent m_gesturePress = nullptr;
    GhosttySelectionGestureEvent m_gestureDrag = nullptr;
    GhosttySelectionGestureEvent m_gestureRelease = nullptr;
    GhosttySelectionGestureEvent m_gestureAutoscroll = nullptr;
    // Created with the first needle.
    GhosttySearch m_search = nullptr;
    bool m_ready = false;

    OutputCallback m_onOutput;
    TitleCallback m_onTitle;
    ClipboardCallback m_onClipboard;
    HostEventCallback m_onHostEvent;

    int m_columns = kDefaultColumns;
    int m_rows = kDefaultRows;
    QSize m_cellPixelSize;
    bool m_wideOverscan = true;
    QColor m_defaultForeground;
    QColor m_defaultBackground;
    QVector<QColor> m_ansiPalette;
    bool m_win32InputMode = false;
    int m_modifyOtherKeys = 0;
    // Mouse buttons held, for motion reports in button-event mode.
    int m_pressedMouseButtons = 0;
    // The shell has marked a prompt (OSC 133 A): prompt jumps have targets.
    bool m_promptMarks = false;
    // The last selection drag's surface position, for autoscroll ticks.
    GhosttySurfacePosition m_dragPosition{};
    QString m_searchNeedle;
    QByteArray m_xtversion;

    // Converted rows by id, from the previous frame.
    QHash<TerminalRowId, CachedLine> m_lines;
    quint64 m_nextRevision = 1;
    quint64 m_graphicsRevision = 1;
    quint64 m_placementsGeneration = 0;
    // Pixels per image version and source rectangle, kept while a placement
    // shows them.
    QHash<QString, std::shared_ptr<const GraphicResource>> m_graphicResources;
};

} // namespace span::terminal
