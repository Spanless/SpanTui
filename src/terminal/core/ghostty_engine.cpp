#include "terminal/core/ghostty_engine.h"
#include "core/core_constants.h"
#include "terminal/core/terminal_win32_input.h"
#include "terminal/terminal_constants.h"

#include <QImage>
#include <QRect>
#include <QSet>
#include <QVarLengthArray>

#include <algorithm>
#include <array>
#include <cstring>
#include <iterator>
#include <limits>
#include <utility>

namespace span::terminal {

namespace {

// Scrollback memory budget. libghostty keeps rows in ~400 KiB pages; the
// row limit (setMaxScrollback) usually applies first.
constexpr size_t kScrollbackMaxBytes = 128 * 1024 * 1024;
// OSC 52 payloads larger than this are refused.
constexpr size_t kClipboardWriteMaxBytes = 8 * 1024 * 1024;
// Search steps per advanceSearch(): enough for a screenful of pages, small
// enough that a huge history never stalls the worker between frames.
constexpr int kSearchTicksPerStep = 64;

template <typename Callback> void setCallback(GhosttyTerminal terminal, GhosttyTerminalOption option, Callback callback)
{
    ghostty_terminal_set(terminal, option, reinterpret_cast<const void *>(callback));
}

GhosttyColorRgb toRgb(const QColor &color)
{
    const QColor rgb = color.toRgb();
    return {uint8_t(rgb.red()), uint8_t(rgb.green()), uint8_t(rgb.blue())};
}

QColor toColor(GhosttyColorRgb rgb)
{
    return QColor(rgb.r, rgb.g, rgb.b);
}

bool sameColor(GhosttyColorRgb rgb, const QColor &color)
{
    return color.isValid() && rgb.r == color.red() && rgb.g == color.green() && rgb.b == color.blue();
}

QString fromGhosttyString(GhosttyString text)
{
    if (!text.ptr || text.len == 0) return {};
    return QString::fromUtf8(reinterpret_cast<const char *>(text.ptr), qsizetype(text.len));
}

// PNG Kitty payloads (f=100). libghostty has no decoder of its own; the
// option is process-wide, so every engine shares this one.
bool decodePng(void *, const GhosttyAllocator *allocator, const uint8_t *data, size_t length, GhosttySysImage *out)
{
    if (length > size_t(std::numeric_limits<int>::max())) return false;
    QImage image = QImage::fromData(data, int(length), "PNG");
    if (image.isNull()) return false;
    image = image.convertToFormat(QImage::Format_RGBA8888);
    const size_t rowBytes = size_t(image.width()) * 4;
    const size_t total = rowBytes * size_t(image.height());
    uint8_t *pixels = ghostty_alloc(allocator, total);
    if (!pixels) return false;
    for (int y = 0; y < image.height(); ++y)
        std::memcpy(pixels + size_t(y) * rowBytes, image.constScanLine(y), rowBytes);
    out->width = uint32_t(image.width());
    out->height = uint32_t(image.height());
    out->data = pixels;
    out->data_len = total;
    return true;
}

void installPngDecoder()
{
    static const bool installed = [] {
        const GhosttySysDecodePngFn decoder = &decodePng;
        ghostty_sys_set(GHOSTTY_SYS_OPT_DECODE_PNG, reinterpret_cast<const void *>(decoder));
        return true;
    }();
    Q_UNUSED(installed);
}

GhosttyMods toMods(Qt::KeyboardModifiers modifiers)
{
    GhosttyMods mods = 0;
    if (modifiers.testFlag(Qt::ShiftModifier)) mods |= GHOSTTY_MODS_SHIFT;
    if (modifiers.testFlag(Qt::ControlModifier)) mods |= GHOSTTY_MODS_CTRL;
    if (modifiers.testFlag(Qt::AltModifier)) mods |= GHOSTTY_MODS_ALT;
    if (modifiers.testFlag(Qt::MetaModifier)) mods |= GHOSTTY_MODS_SUPER;
    return mods;
}

GhosttyKey namedKey(TerminalKey key, int functionNumber, bool keypad)
{
    switch (key) {
    case TerminalKey::Enter: return keypad ? GHOSTTY_KEY_NUMPAD_ENTER : GHOSTTY_KEY_ENTER;
    case TerminalKey::Tab: return GHOSTTY_KEY_TAB;
    case TerminalKey::Backspace: return GHOSTTY_KEY_BACKSPACE;
    case TerminalKey::Escape: return GHOSTTY_KEY_ESCAPE;
    case TerminalKey::Up: return GHOSTTY_KEY_ARROW_UP;
    case TerminalKey::Down: return GHOSTTY_KEY_ARROW_DOWN;
    case TerminalKey::Left: return GHOSTTY_KEY_ARROW_LEFT;
    case TerminalKey::Right: return GHOSTTY_KEY_ARROW_RIGHT;
    case TerminalKey::Insert: return GHOSTTY_KEY_INSERT;
    case TerminalKey::Delete: return GHOSTTY_KEY_DELETE;
    case TerminalKey::Home: return GHOSTTY_KEY_HOME;
    case TerminalKey::End: return GHOSTTY_KEY_END;
    case TerminalKey::PageUp: return GHOSTTY_KEY_PAGE_UP;
    case TerminalKey::PageDown: return GHOSTTY_KEY_PAGE_DOWN;
    case TerminalKey::Function:
        if (functionNumber >= 1 && functionNumber <= 25) return GhosttyKey(GHOSTTY_KEY_F1 + functionNumber - 1);
        return GHOSTTY_KEY_UNIDENTIFIED;
    }
    return GHOSTTY_KEY_UNIDENTIFIED;
}

// The physical key behind a character, from the Windows virtual key when
// there is one (layout-independent, as GhosttyKey is), else from the
// unshifted ASCII character.
GhosttyKey characterKey(const TerminalKeyStroke &stroke)
{
    const quint16 vk = stroke.native.virtualKey;
    if (vk >= 'A' && vk <= 'Z') return GhosttyKey(GHOSTTY_KEY_A + (vk - 'A'));
    if (vk >= '0' && vk <= '9') return GhosttyKey(GHOSTTY_KEY_DIGIT_0 + (vk - '0'));
    if (vk >= 0x60 && vk <= 0x69) return GhosttyKey(GHOSTTY_KEY_NUMPAD_0 + (vk - 0x60));
    switch (vk) {
    case 0x20: return GHOSTTY_KEY_SPACE;
    case 0x6A: return GHOSTTY_KEY_NUMPAD_MULTIPLY;
    case 0x6B: return GHOSTTY_KEY_NUMPAD_ADD;
    case 0x6C: return GHOSTTY_KEY_NUMPAD_SEPARATOR;
    case 0x6D: return GHOSTTY_KEY_NUMPAD_SUBTRACT;
    case 0x6E: return GHOSTTY_KEY_NUMPAD_DECIMAL;
    case 0x6F: return GHOSTTY_KEY_NUMPAD_DIVIDE;
    case 0xBA: return GHOSTTY_KEY_SEMICOLON;
    case 0xBB: return GHOSTTY_KEY_EQUAL;
    case 0xBC: return GHOSTTY_KEY_COMMA;
    case 0xBD: return GHOSTTY_KEY_MINUS;
    case 0xBE: return GHOSTTY_KEY_PERIOD;
    case 0xBF: return GHOSTTY_KEY_SLASH;
    case 0xC0: return GHOSTTY_KEY_BACKQUOTE;
    case 0xDB: return GHOSTTY_KEY_BRACKET_LEFT;
    case 0xDC: return GHOSTTY_KEY_BACKSLASH;
    case 0xDD: return GHOSTTY_KEY_BRACKET_RIGHT;
    case 0xDE: return GHOSTTY_KEY_QUOTE;
    case 0xE2: return GHOSTTY_KEY_INTL_BACKSLASH;
    default: break;
    }
    const int codepoint =
        stroke.textKey.baseLayoutCodepoint ? stroke.textKey.baseLayoutCodepoint : stroke.textKey.codepoint;
    if (stroke.keypad && codepoint >= '0' && codepoint <= '9')
        return GhosttyKey(GHOSTTY_KEY_NUMPAD_0 + (codepoint - '0'));
    if (codepoint >= 'a' && codepoint <= 'z') return GhosttyKey(GHOSTTY_KEY_A + (codepoint - 'a'));
    if (codepoint >= '0' && codepoint <= '9') return GhosttyKey(GHOSTTY_KEY_DIGIT_0 + (codepoint - '0'));
    switch (codepoint) {
    case ' ': return GHOSTTY_KEY_SPACE;
    case ';': return GHOSTTY_KEY_SEMICOLON;
    case '=': return GHOSTTY_KEY_EQUAL;
    case ',': return GHOSTTY_KEY_COMMA;
    case '-': return GHOSTTY_KEY_MINUS;
    case '.': return GHOSTTY_KEY_PERIOD;
    case '/': return GHOSTTY_KEY_SLASH;
    case '`': return GHOSTTY_KEY_BACKQUOTE;
    case '[': return GHOSTTY_KEY_BRACKET_LEFT;
    case '\\': return GHOSTTY_KEY_BACKSLASH;
    case ']': return GHOSTTY_KEY_BRACKET_RIGHT;
    case '\'': return GHOSTTY_KEY_QUOTE;
    default: return GHOSTTY_KEY_UNIDENTIFIED;
    }
}

UnderlineStyle underlineStyle(int underline)
{
    switch (underline) {
    case GHOSTTY_SGR_UNDERLINE_SINGLE: return UnderlineStyle::Single;
    case GHOSTTY_SGR_UNDERLINE_DOUBLE: return UnderlineStyle::Double;
    case GHOSTTY_SGR_UNDERLINE_CURLY: return UnderlineStyle::Curly;
    case GHOSTTY_SGR_UNDERLINE_DOTTED: return UnderlineStyle::Dotted;
    case GHOSTTY_SGR_UNDERLINE_DASHED: return UnderlineStyle::Dashed;
    default: return UnderlineStyle::None;
    }
}

GhosttyTerminalCursorStyle cursorStyle(CursorShape shape)
{
    switch (shape) {
    case CursorShape::Underline: return GHOSTTY_TERMINAL_CURSOR_STYLE_UNDERLINE;
    case CursorShape::Bar: return GHOSTTY_TERMINAL_CURSOR_STYLE_BAR;
    case CursorShape::Block: break;
    }
    return GHOSTTY_TERMINAL_CURSOR_STYLE_BLOCK;
}

// Kitty pixels in the stored format, as an image the view can upload.
QImage kittyImage(GhosttyKittyGraphicsImage image)
{
    uint32_t width = 0;
    uint32_t height = 0;
    GhosttyKittyImageFormat format = GHOSTTY_KITTY_IMAGE_FORMAT_RGBA;
    const uint8_t *data = nullptr;
    size_t length = 0;
    if (ghostty_kitty_graphics_image_get(image, GHOSTTY_KITTY_IMAGE_DATA_WIDTH, &width) != GHOSTTY_SUCCESS
        || ghostty_kitty_graphics_image_get(image, GHOSTTY_KITTY_IMAGE_DATA_HEIGHT, &height) != GHOSTTY_SUCCESS
        || ghostty_kitty_graphics_image_get(image, GHOSTTY_KITTY_IMAGE_DATA_FORMAT, &format) != GHOSTTY_SUCCESS
        || ghostty_kitty_graphics_image_get(image, GHOSTTY_KITTY_IMAGE_DATA_DATA_PTR, &data) != GHOSTTY_SUCCESS
        || ghostty_kitty_graphics_image_get(image, GHOSTTY_KITTY_IMAGE_DATA_DATA_LEN, &length) != GHOSTTY_SUCCESS
        || !data || width == 0 || height == 0) {
        return {};
    }
    const auto fits = [&](size_t bytesPerPixel) { return length >= size_t(width) * size_t(height) * bytesPerPixel; };
    switch (format) {
    case GHOSTTY_KITTY_IMAGE_FORMAT_RGBA:
        if (!fits(4)) return {};
        return QImage(data, int(width), int(height), qsizetype(width) * 4, QImage::Format_RGBA8888).copy();
    case GHOSTTY_KITTY_IMAGE_FORMAT_RGB:
        if (!fits(3)) return {};
        return QImage(data, int(width), int(height), qsizetype(width) * 3, QImage::Format_RGB888).copy();
    case GHOSTTY_KITTY_IMAGE_FORMAT_GRAY:
        if (!fits(1)) return {};
        return QImage(data, int(width), int(height), qsizetype(width), QImage::Format_Grayscale8).copy();
    case GHOSTTY_KITTY_IMAGE_FORMAT_GRAY_ALPHA: {
        if (!fits(2)) return {};
        QImage result(int(width), int(height), QImage::Format_RGBA8888);
        for (uint32_t y = 0; y < height; ++y) {
            const uint8_t *source = data + size_t(y) * width * 2;
            uint8_t *target = result.scanLine(int(y));
            for (uint32_t x = 0; x < width; ++x) {
                target[x * 4] = target[x * 4 + 1] = target[x * 4 + 2] = source[x * 2];
                target[x * 4 + 3] = source[x * 2 + 1];
            }
        }
        return result;
    }
    case GHOSTTY_KITTY_IMAGE_FORMAT_PNG:
        if (length > size_t(std::numeric_limits<int>::max())) return {};
        return QImage::fromData(data, int(length), "PNG");
    default: return {};
    }
}

} // namespace

GhosttyEngine::GhosttyEngine(int columns, int rows)
    : m_columns(std::clamp(columns, 1, int(std::numeric_limits<uint16_t>::max()))),
      m_rows(std::clamp(rows, 1, int(std::numeric_limits<uint16_t>::max())))
{
    installPngDecoder();
    m_xtversion = QByteArray(span::core::kProductName.data(), span::core::kProductName.size());
#ifdef SPANTUI_VERSION
    m_xtversion += " " SPANTUI_VERSION;
#endif
    if (ghostty_terminal_new(nullptr, &m_terminal, uint16_t(m_columns), uint16_t(m_rows)) != GHOSTTY_SUCCESS) {
        m_terminal = nullptr;
        return;
    }
    ghostty_terminal_set(m_terminal, GHOSTTY_TERMINAL_OPT_USERDATA, this);
    setCallback<GhosttyTerminalWritePtyFn>(m_terminal, GHOSTTY_TERMINAL_OPT_WRITE_PTY, &writePtyCallback);
    setCallback<GhosttyTerminalBellFn>(m_terminal, GHOSTTY_TERMINAL_OPT_BELL, &bellCallback);
    setCallback<GhosttyTerminalXtversionFn>(m_terminal, GHOSTTY_TERMINAL_OPT_XTVERSION, &xtversionCallback);
    setCallback<GhosttyTerminalTitleChangedFn>(m_terminal, GHOSTTY_TERMINAL_OPT_TITLE_CHANGED, &titleChangedCallback);
    setCallback<GhosttyTerminalSizeFn>(m_terminal, GHOSTTY_TERMINAL_OPT_SIZE, &sizeCallback);
    setCallback<GhosttyTerminalColorSchemeFn>(m_terminal, GHOSTTY_TERMINAL_OPT_COLOR_SCHEME, &colorSchemeCallback);
    setCallback<GhosttyTerminalClipboardWriteFn>(m_terminal, GHOSTTY_TERMINAL_OPT_CLIPBOARD_WRITE,
                                                 &clipboardWriteCallback);
    setCallback<GhosttyTerminalDesktopNotificationFn>(m_terminal, GHOSTTY_TERMINAL_OPT_DESKTOP_NOTIFICATION,
                                                      &notificationCallback);
    setCallback<GhosttyTerminalProgressReportFn>(m_terminal, GHOSTTY_TERMINAL_OPT_PROGRESS_REPORT, &progressCallback);
    setCallback<GhosttyTerminalSemanticPromptFn>(m_terminal, GHOSTTY_TERMINAL_OPT_SEMANTIC_PROMPT,
                                                 &semanticPromptCallback);

    const size_t scrollbackBytes = kScrollbackMaxBytes;
    ghostty_terminal_set(m_terminal, GHOSTTY_TERMINAL_OPT_SCROLLBACK_MAX_BYTES, &scrollbackBytes);
    const size_t clipboardBytes = kClipboardWriteMaxBytes;
    ghostty_terminal_set(m_terminal, GHOSTTY_TERMINAL_OPT_CLIPBOARD_WRITE_MAX_BYTES, &clipboardBytes);
    // ConPTY repaints the screen after every resize; rows pulled back out of
    // history would be painted over twice.
    const bool disabled = false;
    ghostty_terminal_set(m_terminal, GHOSTTY_TERMINAL_OPT_RESIZE_PULL_SCROLLBACK, &disabled);
    // Title reports (CSI 21 t) let a child echo attacker-controlled text
    // back as input.
    ghostty_terminal_set(m_terminal, GHOSTTY_TERMINAL_OPT_TITLE_REPORT, &disabled);
    ghostty_terminal_set(m_terminal, GHOSTTY_TERMINAL_OPT_GLYPH_PROTOCOL, &disabled);
    // Kitty images arrive only inline (t=d): file and shared-memory media
    // would let a child make the terminal read paths of its choosing.
    ghostty_terminal_set(m_terminal, GHOSTTY_TERMINAL_OPT_KITTY_IMAGE_MEDIUM_FILE, &disabled);
    ghostty_terminal_set(m_terminal, GHOSTTY_TERMINAL_OPT_KITTY_IMAGE_MEDIUM_SHARED_MEM, &disabled);
    ghostty_terminal_set(m_terminal, GHOSTTY_TERMINAL_OPT_KITTY_IMAGE_MEDIUM_TEMP_FILE, nullptr);
    const bool blink = true;
    ghostty_terminal_set(m_terminal, GHOSTTY_TERMINAL_OPT_DEFAULT_CURSOR_BLINK, &blink);
    setMaxScrollback(kDefaultMaxScrollbackLines);

    bool ok = ghostty_render_state_new(nullptr, &m_renderState) == GHOSTTY_SUCCESS
        && ghostty_render_state_row_iterator_new(nullptr, &m_rowIterator) == GHOSTTY_SUCCESS
        && ghostty_render_state_row_cells_new(nullptr, &m_cells) == GHOSTTY_SUCCESS
        && ghostty_render_state_image_iterator_new(nullptr, &m_images) == GHOSTTY_SUCCESS
        && ghostty_key_encoder_new(nullptr, &m_keyEncoder) == GHOSTTY_SUCCESS
        && ghostty_key_event_new(nullptr, &m_keyEvent) == GHOSTTY_SUCCESS
        && ghostty_mouse_encoder_new(nullptr, &m_mouseEncoder) == GHOSTTY_SUCCESS
        && ghostty_mouse_event_new(nullptr, &m_mouseEvent) == GHOSTTY_SUCCESS
        && ghostty_selection_gesture_new(nullptr, &m_gesture) == GHOSTTY_SUCCESS
        && ghostty_selection_gesture_event_new(nullptr, &m_gesturePress, GHOSTTY_SELECTION_GESTURE_EVENT_TYPE_PRESS)
            == GHOSTTY_SUCCESS
        && ghostty_selection_gesture_event_new(nullptr, &m_gestureDrag, GHOSTTY_SELECTION_GESTURE_EVENT_TYPE_DRAG)
            == GHOSTTY_SUCCESS
        && ghostty_selection_gesture_event_new(nullptr, &m_gestureRelease, GHOSTTY_SELECTION_GESTURE_EVENT_TYPE_RELEASE)
            == GHOSTTY_SUCCESS
        && ghostty_selection_gesture_event_new(nullptr, &m_gestureAutoscroll,
                                               GHOSTTY_SELECTION_GESTURE_EVENT_TYPE_AUTOSCROLL_TICK)
            == GHOSTTY_SUCCESS;
    if (!ok) return;
    applyOverscan();
    m_ready = true;
}

void GhosttyEngine::applyOverscan()
{
    const uint16_t rows = m_wideOverscan ? uint16_t(m_rows) : uint16_t(1);
    const GhosttyRenderStateOverscan overscan{rows, rows};
    ghostty_render_state_set(m_renderState, GHOSTTY_RENDER_STATE_OPTION_OVERSCAN, &overscan);
}

void GhosttyEngine::setWideOverscan(bool wide)
{
    if (!m_ready || wide == m_wideOverscan) return;
    m_wideOverscan = wide;
    applyOverscan();
}

GhosttyEngine::~GhosttyEngine()
{
    ghostty_search_free(m_search);
    ghostty_selection_gesture_event_free(m_gestureAutoscroll);
    ghostty_selection_gesture_event_free(m_gestureRelease);
    ghostty_selection_gesture_event_free(m_gestureDrag);
    ghostty_selection_gesture_event_free(m_gesturePress);
    if (m_gesture) ghostty_selection_gesture_free(m_gesture, m_terminal);
    ghostty_mouse_event_free(m_mouseEvent);
    ghostty_mouse_encoder_free(m_mouseEncoder);
    ghostty_key_event_free(m_keyEvent);
    ghostty_key_encoder_free(m_keyEncoder);
    ghostty_render_state_image_iterator_free(m_images);
    ghostty_render_state_row_cells_free(m_cells);
    ghostty_render_state_row_iterator_free(m_rowIterator);
    ghostty_render_state_free(m_renderState);
    ghostty_terminal_free(m_terminal);
}

void GhosttyEngine::writePtyCallback(GhosttyTerminal, void *user, const uint8_t *data, size_t length)
{
    if (!data || length == 0) return;
    static_cast<GhosttyEngine *>(user)->respond(QByteArray(reinterpret_cast<const char *>(data), qsizetype(length)));
}

void GhosttyEngine::bellCallback(GhosttyTerminal, void *user)
{
    static_cast<GhosttyEngine *>(user)->emitHostEvent({});
}

void GhosttyEngine::titleChangedCallback(GhosttyTerminal terminal, void *user)
{
    auto *engine = static_cast<GhosttyEngine *>(user);
    GhosttyString title{};
    if (ghostty_terminal_get(terminal, GHOSTTY_TERMINAL_DATA_TITLE, &title) != GHOSTTY_SUCCESS) return;
    if (engine->m_onTitle) engine->m_onTitle(fromGhosttyString(title));
}

bool GhosttyEngine::sizeCallback(GhosttyTerminal, void *user, GhosttySizeReportSize *size)
{
    auto *engine = static_cast<GhosttyEngine *>(user);
    size->rows = uint16_t(engine->m_rows);
    size->columns = uint16_t(engine->m_columns);
    size->cell_width = uint32_t(std::max(0, engine->m_cellPixelSize.width()));
    size->cell_height = uint32_t(std::max(0, engine->m_cellPixelSize.height()));
    return true;
}

bool GhosttyEngine::colorSchemeCallback(GhosttyTerminal, void *user, GhosttyColorScheme *scheme)
{
    *scheme =
        static_cast<GhosttyEngine *>(user)->darkSurface() ? GHOSTTY_COLOR_SCHEME_DARK : GHOSTTY_COLOR_SCHEME_LIGHT;
    return true;
}

void GhosttyEngine::clipboardWriteCallback(GhosttyTerminal, void *user, const GhosttyClipboardWrite *write)
{
    auto *engine = static_cast<GhosttyEngine *>(user);
    GhosttyClipboardWriteReply reply = GHOSTTY_INIT_SIZED(GhosttyClipboardWriteReply);
    reply.result = GHOSTTY_CLIPBOARD_WRITE_RESULT_DENIED;
    if (engine->m_onClipboard) {
        // Plain text is the one representation the host clipboard takes.
        for (size_t index = 0; index < write->contents_len; ++index) {
            const GhosttyClipboardContent &content = write->contents[index];
            const QString mime = fromGhosttyString(content.mime);
            if (!mime.isEmpty() && !mime.startsWith(QLatin1String("text/plain"))) continue;
            engine->m_onClipboard(fromGhosttyString(content.data));
            reply.result = GHOSTTY_CLIPBOARD_WRITE_RESULT_SUCCESS;
            break;
        }
        if (reply.result != GHOSTTY_CLIPBOARD_WRITE_RESULT_SUCCESS)
            reply.result = GHOSTTY_CLIPBOARD_WRITE_RESULT_UNSUPPORTED;
    }
    write->reply(write, &reply);
}

void GhosttyEngine::notificationCallback(GhosttyTerminal, void *user,
                                         const GhosttyTerminalDesktopNotification *notification)
{
    TerminalHostEvent event;
    event.kind = TerminalHostEvent::Kind::Notification;
    event.title = fromGhosttyString(notification->title);
    event.text = fromGhosttyString(notification->body);
    static_cast<GhosttyEngine *>(user)->emitHostEvent(event);
}

void GhosttyEngine::progressCallback(GhosttyTerminal, void *user, const GhosttyTerminalProgressReport *report)
{
    TerminalHostEvent event;
    event.kind = TerminalHostEvent::Kind::Progress;
    switch (report->state) {
    case GHOSTTY_TERMINAL_PROGRESS_STATE_SET: event.progress = TerminalProgressState::Normal; break;
    case GHOSTTY_TERMINAL_PROGRESS_STATE_ERROR: event.progress = TerminalProgressState::Error; break;
    case GHOSTTY_TERMINAL_PROGRESS_STATE_INDETERMINATE: event.progress = TerminalProgressState::Indeterminate; break;
    case GHOSTTY_TERMINAL_PROGRESS_STATE_PAUSE: event.progress = TerminalProgressState::Paused; break;
    default: event.progress = TerminalProgressState::None; break;
    }
    event.progressValue = std::clamp(int(report->progress), 0, 100);
    static_cast<GhosttyEngine *>(user)->emitHostEvent(event);
}

void GhosttyEngine::semanticPromptCallback(GhosttyTerminal, void *user, const GhosttyTerminalSemanticPrompt *event)
{
    if (event->kind == GHOSTTY_SEMANTIC_PROMPT_PROMPT_START) static_cast<GhosttyEngine *>(user)->m_promptMarks = true;
}

GhosttyString GhosttyEngine::xtversionCallback(GhosttyTerminal, void *user)
{
    const QByteArray &version = static_cast<GhosttyEngine *>(user)->m_xtversion;
    return {reinterpret_cast<const uint8_t *>(version.constData()), size_t(version.size())};
}

void GhosttyEngine::respond(const QByteArray &bytes)
{
    if (m_onOutput && !bytes.isEmpty()) m_onOutput(bytes);
}

void GhosttyEngine::emitHostEvent(const TerminalHostEvent &event)
{
    if (m_onHostEvent) m_onHostEvent(event);
}

bool GhosttyEngine::modeEnabled(GhosttyMode mode) const
{
    GhosttyTerminalModeConfig config{mode, false};
    return ghostty_terminal_get(m_terminal, GHOSTTY_TERMINAL_DATA_MODE, &config) == GHOSTTY_SUCCESS && config.value;
}

bool GhosttyEngine::darkSurface() const
{
    // The child's OSC 11 counts: it decides what the user actually sees.
    GhosttyColorRgb background{};
    QColor color = m_defaultBackground;
    if (m_terminal
        && ghostty_terminal_get(m_terminal, GHOSTTY_TERMINAL_DATA_COLOR_BACKGROUND, &background) == GHOSTTY_SUCCESS) {
        color = toColor(background);
    }
    if (!color.isValid()) return true;
    return 0.2126 * color.red() + 0.7152 * color.green() + 0.0722 * color.blue() < 128.0;
}

void GhosttyEngine::reportColorScheme()
{
    char buffer[32];
    size_t written = 0;
    if (ghostty_color_scheme_report_encode(darkSurface() ? GHOSTTY_COLOR_SCHEME_DARK : GHOSTTY_COLOR_SCHEME_LIGHT,
                                           buffer, sizeof(buffer), &written)
        == GHOSTTY_SUCCESS) {
        respond(QByteArray(buffer, qsizetype(written)));
    }
}

void GhosttyEngine::applyColors()
{
    if (m_defaultForeground.isValid()) {
        const GhosttyColorRgb foreground = toRgb(m_defaultForeground);
        ghostty_terminal_set(m_terminal, GHOSTTY_TERMINAL_OPT_COLOR_FOREGROUND, &foreground);
    } else {
        ghostty_terminal_set(m_terminal, GHOSTTY_TERMINAL_OPT_COLOR_FOREGROUND, nullptr);
    }
    if (m_defaultBackground.isValid()) {
        const GhosttyColorRgb background = toRgb(m_defaultBackground);
        ghostty_terminal_set(m_terminal, GHOSTTY_TERMINAL_OPT_COLOR_BACKGROUND, &background);
    } else {
        ghostty_terminal_set(m_terminal, GHOSTTY_TERMINAL_OPT_COLOR_BACKGROUND, nullptr);
    }
    GhosttyColorRgb palette[256];
    ghostty_color_palette_default(palette);
    for (int index = 0; index < m_ansiPalette.size(); ++index) palette[index] = toRgb(m_ansiPalette.at(index));
    ghostty_terminal_set(m_terminal, GHOSTTY_TERMINAL_OPT_COLOR_PALETTE, palette);
    // Which cells keep semantic colours depends on the theme: convert again.
    m_lines.clear();
}

GhosttyColorRgb GhosttyEngine::basePaletteColor(int index) const
{
    if (index < m_ansiPalette.size()) return toRgb(m_ansiPalette.at(index));
    static const std::array<GhosttyColorRgb, 256> defaults = [] {
        std::array<GhosttyColorRgb, 256> palette{};
        ghostty_color_palette_default(palette.data());
        return palette;
    }();
    return defaults[size_t(index)];
}

void GhosttyEngine::setCellPixelSize(QSize size)
{
    const QSize next = size.isValid() && !size.isEmpty() ? size : QSize();
    if (next == m_cellPixelSize) return;
    m_cellPixelSize = next;
    if (m_ready) resize(m_columns, m_rows);
}

void GhosttyEngine::setDefaultColors(const QColor &foreground, const QColor &background)
{
    const QColor nextForeground = foreground.isValid() ? foreground.toRgb() : QColor();
    const QColor nextBackground = background.isValid() ? background.toRgb() : QColor();
    if (!m_ready || (nextForeground == m_defaultForeground && nextBackground == m_defaultBackground)) return;
    const bool wasDark = darkSurface();
    m_defaultForeground = nextForeground;
    m_defaultBackground = nextBackground;
    applyColors();
    if (darkSurface() != wasDark && modeEnabled(GHOSTTY_MODE_COLOR_SCHEME_REPORT)) reportColorScheme();
}

void GhosttyEngine::setAnsiPalette(const QVector<QColor> &palette)
{
    if (!m_ready || palette.size() != 16) return;
    QVector<QColor> next;
    next.reserve(16);
    for (const QColor &color : palette) {
        if (!color.isValid()) return;
        next.append(color.toRgb());
    }
    if (next == m_ansiPalette) return;
    m_ansiPalette = std::move(next);
    applyColors();
}

void GhosttyEngine::setDefaultCursorShape(CursorShape shape)
{
    if (!m_ready) return;
    const GhosttyTerminalCursorStyle style = cursorStyle(shape);
    ghostty_terminal_set(m_terminal, GHOSTTY_TERMINAL_OPT_DEFAULT_CURSOR_STYLE, &style);
}

void GhosttyEngine::setMaxScrollback(int lines)
{
    if (!m_terminal) return;
    if (lines < 0) {
        ghostty_terminal_set(m_terminal, GHOSTTY_TERMINAL_OPT_SCROLLBACK_MAX_LINES, nullptr);
        return;
    }
    const size_t limit = size_t(lines);
    ghostty_terminal_set(m_terminal, GHOSTTY_TERMINAL_OPT_SCROLLBACK_MAX_LINES, &limit);
}

void GhosttyEngine::feed(QByteArrayView bytes)
{
    if (!m_ready || bytes.isEmpty()) return;
    ghostty_terminal_vt_write(m_terminal, reinterpret_cast<const uint8_t *>(bytes.data()), size_t(bytes.size()));
}

void GhosttyEngine::resize(int columns, int rows)
{
    if (!m_ready) return;
    m_columns = std::clamp(columns, 1, int(std::numeric_limits<uint16_t>::max()));
    m_rows = std::clamp(rows, 1, int(std::numeric_limits<uint16_t>::max()));
    ghostty_terminal_resize(m_terminal, uint16_t(m_columns), uint16_t(m_rows),
                            uint32_t(std::max(0, m_cellPixelSize.width())),
                            uint32_t(std::max(0, m_cellPixelSize.height())));
    applyOverscan();
    m_lines.clear();
}

void GhosttyEngine::clearDisplay()
{
    if (!m_ready) return;
    // The same clear ConPTY makes (keepCursorRow): the cursor's row becomes
    // the top row, every other row and the history go, and the cursor keeps
    // its column. The child's later output then lands where it expects.
    uint16_t column = 0;
    uint16_t row = 0;
    ghostty_terminal_get(m_terminal, GHOSTTY_TERMINAL_DATA_CURSOR_X, &column);
    ghostty_terminal_get(m_terminal, GHOSTTY_TERMINAL_DATA_CURSOR_Y, &row);
    QByteArray sequence;
    if (row > 0) sequence += "\x1b[" + QByteArray::number(row) + 'S';
    if (m_rows > 1) sequence += "\x1b[2;1H\x1b[J";
    sequence += "\x1b[1;" + QByteArray::number(column + 1) + 'H';
    // ED 3 last: the scroll above may have pushed rows into the history.
    sequence += "\x1b[3J";
    feed(sequence);
}

bool GhosttyEngine::win32Channel() const
{
    if (!m_win32InputMode || m_modifyOtherKeys > 0) return false;
    uint8_t kittyFlags = 0;
    ghostty_terminal_get(m_terminal, GHOSTTY_TERMINAL_DATA_KITTY_KEYBOARD_FLAGS, &kittyFlags);
    return kittyFlags == 0;
}

void GhosttyEngine::sendKeyStroke(const TerminalKeyStroke &stroke)
{
    if (!m_ready) return;
    if (win32Channel()) {
        if (stroke.native.virtualKey != 0) {
            respond(TerminalWin32Input::keyRecords(stroke));
            return;
        }
        // No physical key behind it (an IME placeholder or a synthetic
        // event). Plain text is then committed text; anything else falls
        // back to VT encoding.
        const bool shortcut = stroke.modifiers & (Qt::ControlModifier | Qt::AltModifier | Qt::MetaModifier);
        if (!stroke.key && !shortcut) {
            if (stroke.event != TerminalKeyEvent::Release) respond(TerminalWin32Input::text(stroke.text));
            return;
        }
    }
    respond(encodeKey(stroke));
}

QByteArray GhosttyEngine::encodeKey(const TerminalKeyStroke &stroke)
{
    ghostty_key_encoder_setopt_from_terminal(m_keyEncoder, m_terminal);
    ghostty_key_event_set_action(m_keyEvent,
                                 stroke.event == TerminalKeyEvent::Release      ? GHOSTTY_KEY_ACTION_RELEASE
                                     : stroke.event == TerminalKeyEvent::Repeat ? GHOSTTY_KEY_ACTION_REPEAT
                                                                                : GHOSTTY_KEY_ACTION_PRESS);
    const GhosttyMods mods = toMods(stroke.modifiers);
    ghostty_key_event_set_mods(m_keyEvent, mods);
    ghostty_key_event_set_composing(m_keyEvent, false);
    QByteArray utf8;
    if (stroke.key) {
        ghostty_key_event_set_key(m_keyEvent, namedKey(*stroke.key, stroke.functionNumber, stroke.keypad));
        ghostty_key_event_set_consumed_mods(m_keyEvent, 0);
        ghostty_key_event_set_unshifted_codepoint(m_keyEvent, 0);
    } else {
        ghostty_key_event_set_key(m_keyEvent, characterKey(stroke));
        // The encoder derives Ctrl/Alt sequences from the key and mods; the
        // text is the layout's character before them, never a C0 control.
        const bool control =
            !stroke.text.isEmpty() && (stroke.text.front().unicode() < 0x20 || stroke.text.front().unicode() == 0x7f);
        if (!control) utf8 = stroke.text.toUtf8();
        // Shift that produced the character is part of the text, not a
        // modifier the child should see again.
        const bool shiftConsumed = stroke.textKey.shiftedCodepoint != 0;
        ghostty_key_event_set_consumed_mods(m_keyEvent,
                                            shiftConsumed ? GhosttyMods(GHOSTTY_MODS_SHIFT) : GhosttyMods(0));
        int unshifted = stroke.textKey.codepoint;
        if (unshifted == 0 && stroke.text.size() == 1) unshifted = stroke.text.front().toLower().unicode();
        ghostty_key_event_set_unshifted_codepoint(m_keyEvent, uint32_t(std::max(0, unshifted)));
    }
    ghostty_key_event_set_utf8(m_keyEvent, utf8.isEmpty() ? nullptr : utf8.constData(), size_t(utf8.size()));
    char buffer[128];
    size_t written = 0;
    const GhosttyResult result = ghostty_key_encoder_encode(m_keyEncoder, m_keyEvent, buffer, sizeof(buffer), &written);
    ghostty_key_event_set_utf8(m_keyEvent, nullptr, 0);
    if (result == GHOSTTY_SUCCESS) return QByteArray(buffer, qsizetype(written));
    if (result != GHOSTTY_OUT_OF_SPACE) return {};
    QByteArray bytes(qsizetype(written), Qt::Uninitialized);
    ghostty_key_event_set_utf8(m_keyEvent, utf8.isEmpty() ? nullptr : utf8.constData(), size_t(utf8.size()));
    if (ghostty_key_encoder_encode(m_keyEncoder, m_keyEvent, bytes.data(), size_t(bytes.size()), &written)
        != GHOSTTY_SUCCESS) {
        bytes.clear();
    }
    ghostty_key_event_set_utf8(m_keyEvent, nullptr, 0);
    bytes.resize(qsizetype(written));
    return bytes;
}

void GhosttyEngine::sendText(const QString &text)
{
    if (!m_ready || text.isEmpty()) return;
    respond(win32Channel() ? TerminalWin32Input::text(text) : text.toUtf8());
}

void GhosttyEngine::beginPaste()
{
    if (m_ready && modeEnabled(GHOSTTY_MODE_BRACKETED_PASTE)) respond(QByteArrayLiteral("\x1b[200~"));
}

void GhosttyEngine::endPaste()
{
    if (m_ready && modeEnabled(GHOSTTY_MODE_BRACKETED_PASTE)) respond(QByteArrayLiteral("\x1b[201~"));
}

void GhosttyEngine::sendPasteBody(const QString &text)
{
    // Paste is text, never key events: shells expect the literal clipboard
    // characters inside bracketed paste markers.
    sendText(text);
}

bool GhosttyEngine::mouseTracking() const
{
    bool tracking = false;
    return m_ready
        && ghostty_terminal_get(m_terminal, GHOSTTY_TERMINAL_DATA_MOUSE_TRACKING, &tracking) == GHOSTTY_SUCCESS
        && tracking;
}

QSize GhosttyEngine::cellSize() const
{
    return m_cellPixelSize.isEmpty() ? QSize(1, 1) : m_cellPixelSize;
}

void GhosttyEngine::encodeMouse(GhosttyMouseAction action, std::optional<GhosttyMouseButton> button, int row,
                                int column, Qt::KeyboardModifiers modifiers)
{
    ghostty_mouse_encoder_setopt_from_terminal(m_mouseEncoder, m_terminal);
    const QSize cell = cellSize();
    GhosttyMouseEncoderSize size = GHOSTTY_INIT_SIZED(GhosttyMouseEncoderSize);
    size.screen_width = uint32_t(cell.width() * m_columns);
    size.screen_height = uint32_t(cell.height() * m_rows);
    size.cell_width = uint32_t(cell.width());
    size.cell_height = uint32_t(cell.height());
    ghostty_mouse_encoder_setopt(m_mouseEncoder, GHOSTTY_MOUSE_ENCODER_OPT_SIZE, &size);
    const bool anyPressed = m_pressedMouseButtons != 0;
    ghostty_mouse_encoder_setopt(m_mouseEncoder, GHOSTTY_MOUSE_ENCODER_OPT_ANY_BUTTON_PRESSED, &anyPressed);

    ghostty_mouse_event_set_action(m_mouseEvent, action);
    if (button)
        ghostty_mouse_event_set_button(m_mouseEvent, *button);
    else
        ghostty_mouse_event_clear_button(m_mouseEvent);
    ghostty_mouse_event_set_mods(m_mouseEvent, toMods(modifiers));
    // The cell's centre: SGR-pixel reports land inside the cell clicked.
    const int clampedRow = std::clamp(row, 0, m_rows - 1);
    const int clampedColumn = std::clamp(column, 0, m_columns - 1);
    ghostty_mouse_event_set_position(
        m_mouseEvent,
        GhosttyMousePosition{float((clampedColumn + 0.5) * cell.width()), float((clampedRow + 0.5) * cell.height())});
    char buffer[64];
    size_t written = 0;
    if (ghostty_mouse_encoder_encode(m_mouseEncoder, m_mouseEvent, buffer, sizeof(buffer), &written)
        == GHOSTTY_SUCCESS) {
        respond(QByteArray(buffer, qsizetype(written)));
    }
}

void GhosttyEngine::sendMouseMove(int row, int column, Qt::KeyboardModifiers modifiers)
{
    if (!mouseTracking()) return;
    encodeMouse(GHOSTTY_MOUSE_ACTION_MOTION, std::nullopt, row, column, modifiers);
}

void GhosttyEngine::sendMouseButton(TerminalMouseButton button, bool pressed, int row, int column,
                                    Qt::KeyboardModifiers modifiers)
{
    if (!mouseTracking()) return;
    GhosttyMouseButton ghosttyButton = GHOSTTY_MOUSE_BUTTON_LEFT;
    switch (button) {
    case TerminalMouseButton::Left: ghosttyButton = GHOSTTY_MOUSE_BUTTON_LEFT; break;
    case TerminalMouseButton::Middle: ghosttyButton = GHOSTTY_MOUSE_BUTTON_MIDDLE; break;
    case TerminalMouseButton::Right: ghosttyButton = GHOSTTY_MOUSE_BUTTON_RIGHT; break;
    case TerminalMouseButton::WheelUp: ghosttyButton = GHOSTTY_MOUSE_BUTTON_FOUR; break;
    case TerminalMouseButton::WheelDown: ghosttyButton = GHOSTTY_MOUSE_BUTTON_FIVE; break;
    case TerminalMouseButton::WheelLeft: ghosttyButton = GHOSTTY_MOUSE_BUTTON_SIX; break;
    case TerminalMouseButton::WheelRight: ghosttyButton = GHOSTTY_MOUSE_BUTTON_SEVEN; break;
    }
    const bool wheel = int(button) >= int(TerminalMouseButton::WheelUp);
    if (wheel && !pressed) return;
    if (!wheel) {
        const int bit = 1 << int(button);
        m_pressedMouseButtons = pressed ? (m_pressedMouseButtons | bit) : (m_pressedMouseButtons & ~bit);
    }
    encodeMouse(pressed ? GHOSTTY_MOUSE_ACTION_PRESS : GHOSTTY_MOUSE_ACTION_RELEASE, ghosttyButton, row, column,
                modifiers);
}

void GhosttyEngine::setFocused(bool focused)
{
    if (!m_ready || !modeEnabled(GHOSTTY_MODE_FOCUS_EVENT)) return;
    char buffer[8];
    size_t written = 0;
    if (ghostty_focus_encode(focused ? GHOSTTY_FOCUS_GAINED : GHOSTTY_FOCUS_LOST, buffer, sizeof(buffer), &written)
        == GHOSTTY_SUCCESS) {
        respond(QByteArray(buffer, qsizetype(written)));
    }
}

void GhosttyEngine::scrollViewportTo(qint64 row)
{
    if (!m_ready) return;
    GhosttyTerminalScrollViewport scroll{};
    scroll.tag = GHOSTTY_SCROLL_VIEWPORT_ROW;
    scroll.value.row = size_t(std::max<qint64>(0, row));
    ghostty_terminal_scroll_viewport(m_terminal, scroll);
}

void GhosttyEngine::scrollViewportToBottom()
{
    if (!m_ready) return;
    GhosttyTerminalScrollViewport scroll{};
    scroll.tag = GHOSTTY_SCROLL_VIEWPORT_BOTTOM;
    ghostty_terminal_scroll_viewport(m_terminal, scroll);
}

void GhosttyEngine::scrollViewportByPrompts(int delta)
{
    if (!m_ready || delta == 0) return;
    GhosttyTerminalScrollViewport scroll{};
    scroll.tag = GHOSTTY_SCROLL_VIEWPORT_DELTA_PROMPT;
    scroll.value.delta_prompt = intptr_t(delta);
    ghostty_terminal_scroll_viewport(m_terminal, scroll);
}

TerminalScrollbar GhosttyEngine::scrollbar() const
{
    TerminalScrollbar result;
    result.rows = m_rows;
    GhosttyTerminalScrollbar scrollbar{};
    if (m_ready && ghostty_terminal_get(m_terminal, GHOSTTY_TERMINAL_DATA_SCROLLBAR, &scrollbar) == GHOSTTY_SUCCESS) {
        result.total = qint64(scrollbar.total);
        result.offset = qint64(scrollbar.offset);
        result.rows = int(scrollbar.len);
    }
    return result;
}

qint64 GhosttyEngine::scrollbackRows() const
{
    size_t rows = 0;
    if (m_ready) ghostty_terminal_get(m_terminal, GHOSTTY_TERMINAL_DATA_SCROLLBACK_ROWS, &rows);
    return qint64(rows);
}

void GhosttyEngine::compressHistory(bool full)
{
    if (!m_ready) return;
    GhosttyTerminalCompressionResult result{};
    ghostty_terminal_compress(
        m_terminal, full ? GHOSTTY_TERMINAL_COMPRESSION_MODE_FULL : GHOSTTY_TERMINAL_COMPRESSION_MODE_INCREMENTAL,
        &result);
}

std::optional<GhosttyGridRef> GhosttyEngine::screenRef(qint64 screenRow, int column) const
{
    if (screenRow < 0 || screenRow > qint64(std::numeric_limits<uint32_t>::max())) return std::nullopt;
    GhosttyPoint point{};
    point.tag = GHOSTTY_POINT_TAG_SCREEN;
    point.value.coordinate.x = uint16_t(std::clamp(column, 0, m_columns - 1));
    point.value.coordinate.y = uint32_t(screenRow);
    GhosttyGridRef ref = GHOSTTY_INIT_SIZED(GhosttyGridRef);
    if (ghostty_terminal_grid_ref(m_terminal, point, &ref) != GHOSTTY_SUCCESS) return std::nullopt;
    return ref;
}

std::optional<GhosttyPointCoordinate> GhosttyEngine::screenPoint(const GhosttyGridRef &ref) const
{
    GhosttyPointCoordinate coordinate{};
    if (ghostty_terminal_point_from_grid_ref(m_terminal, &ref, GHOSTTY_POINT_TAG_SCREEN, &coordinate)
        != GHOSTTY_SUCCESS) {
        return std::nullopt;
    }
    return coordinate;
}

GhosttySelectionGestureGeometry GhosttyEngine::gestureGeometry() const
{
    const QSize cell = cellSize();
    GhosttySelectionGestureGeometry geometry{};
    geometry.columns = uint32_t(m_columns);
    geometry.cell_width = uint32_t(cell.width());
    geometry.padding_left = 0;
    geometry.screen_height = uint32_t(cell.height() * m_rows);
    return geometry;
}

void GhosttyEngine::installSelection(const GhosttySelection *selection)
{
    ghostty_terminal_set(m_terminal, GHOSTTY_TERMINAL_OPT_SELECTION, selection);
}

bool GhosttyEngine::selectionEvent(const TerminalSelectionEvent &event)
{
    if (!m_ready) return false;
    const qint64 total = scrollbar().total;
    // A drag past the first or last row selects up to it.
    const qint64 screenRow = std::clamp<qint64>(event.offset + event.row, 0, std::max<qint64>(0, total - 1));
    const std::optional<GhosttyGridRef> ref = screenRef(screenRow, event.column);
    const GhosttySurfacePosition position{double(event.x), double(event.y)};
    GhosttySelection selection = GHOSTTY_INIT_SIZED(GhosttySelection);
    switch (event.kind) {
    case TerminalSelectionEvent::Kind::Press: {
        if (!ref) {
            clearSelection();
            return false;
        }
        const uint64_t timeNs = uint64_t(std::max<qint64>(0, event.timeMs)) * 1'000'000u;
        const uint64_t intervalNs = uint64_t(std::max(0, event.repeatIntervalMs)) * 1'000'000u;
        ghostty_selection_gesture_event_set(m_gesturePress, GHOSTTY_SELECTION_GESTURE_EVENT_OPT_REF, &*ref);
        ghostty_selection_gesture_event_set(m_gesturePress, GHOSTTY_SELECTION_GESTURE_EVENT_OPT_POSITION, &position);
        ghostty_selection_gesture_event_set(m_gesturePress, GHOSTTY_SELECTION_GESTURE_EVENT_OPT_TIME_NS, &timeNs);
        ghostty_selection_gesture_event_set(m_gesturePress, GHOSTTY_SELECTION_GESTURE_EVENT_OPT_REPEAT_INTERVAL_NS,
                                            &intervalNs);
        // A single click only anchors; double and triple clicks select a
        // word or a line at once.
        if (ghostty_selection_gesture_event(m_gesture, m_terminal, m_gesturePress, &selection) == GHOSTTY_SUCCESS)
            installSelection(&selection);
        else
            installSelection(nullptr);
        return false;
    }
    case TerminalSelectionEvent::Kind::Drag: {
        if (!ref) return false;
        const GhosttySelectionGestureGeometry geometry = gestureGeometry();
        const bool rectangle = event.rectangle;
        ghostty_selection_gesture_event_set(m_gestureDrag, GHOSTTY_SELECTION_GESTURE_EVENT_OPT_REF, &*ref);
        ghostty_selection_gesture_event_set(m_gestureDrag, GHOSTTY_SELECTION_GESTURE_EVENT_OPT_POSITION, &position);
        ghostty_selection_gesture_event_set(m_gestureDrag, GHOSTTY_SELECTION_GESTURE_EVENT_OPT_GEOMETRY, &geometry);
        ghostty_selection_gesture_event_set(m_gestureDrag, GHOSTTY_SELECTION_GESTURE_EVENT_OPT_RECTANGLE, &rectangle);
        if (ghostty_selection_gesture_event(m_gesture, m_terminal, m_gestureDrag, &selection) == GHOSTTY_SUCCESS)
            installSelection(&selection);
        m_dragPosition = position;
        GhosttySelectionGestureAutoscroll autoscroll = GHOSTTY_SELECTION_GESTURE_AUTOSCROLL_NONE;
        ghostty_selection_gesture_get(m_gesture, m_terminal, GHOSTTY_SELECTION_GESTURE_DATA_AUTOSCROLL, &autoscroll);
        return autoscroll != GHOSTTY_SELECTION_GESTURE_AUTOSCROLL_NONE;
    }
    case TerminalSelectionEvent::Kind::Release:
        ghostty_selection_gesture_event_set(m_gestureRelease, GHOSTTY_SELECTION_GESTURE_EVENT_OPT_REF,
                                            ref ? &*ref : nullptr);
        ghostty_selection_gesture_event(m_gesture, m_terminal, m_gestureRelease, nullptr);
        return false;
    }
    return false;
}

bool GhosttyEngine::selectionAutoscrollTick()
{
    if (!m_ready) return false;
    GhosttySelectionGestureAutoscroll autoscroll = GHOSTTY_SELECTION_GESTURE_AUTOSCROLL_NONE;
    ghostty_selection_gesture_get(m_gesture, m_terminal, GHOSTTY_SELECTION_GESTURE_DATA_AUTOSCROLL, &autoscroll);
    if (autoscroll == GHOSTTY_SELECTION_GESTURE_AUTOSCROLL_NONE) return false;
    const bool up = autoscroll == GHOSTTY_SELECTION_GESTURE_AUTOSCROLL_UP;
    GhosttyTerminalScrollViewport scroll{};
    scroll.tag = GHOSTTY_SCROLL_VIEWPORT_DELTA;
    scroll.value.delta = up ? -1 : 1;
    ghostty_terminal_scroll_viewport(m_terminal, scroll);

    const QSize cell = cellSize();
    GhosttyPointCoordinate viewport{};
    viewport.x = uint16_t(std::clamp(int(m_dragPosition.x / cell.width()), 0, m_columns - 1));
    viewport.y = uint32_t(up ? 0 : m_rows - 1);
    const GhosttySelectionGestureGeometry geometry = gestureGeometry();
    ghostty_selection_gesture_event_set(m_gestureAutoscroll, GHOSTTY_SELECTION_GESTURE_EVENT_OPT_VIEWPORT, &viewport);
    ghostty_selection_gesture_event_set(m_gestureAutoscroll, GHOSTTY_SELECTION_GESTURE_EVENT_OPT_POSITION,
                                        &m_dragPosition);
    ghostty_selection_gesture_event_set(m_gestureAutoscroll, GHOSTTY_SELECTION_GESTURE_EVENT_OPT_GEOMETRY, &geometry);
    GhosttySelection selection = GHOSTTY_INIT_SIZED(GhosttySelection);
    if (ghostty_selection_gesture_event(m_gesture, m_terminal, m_gestureAutoscroll, &selection) == GHOSTTY_SUCCESS)
        installSelection(&selection);
    return true;
}

void GhosttyEngine::selectAll()
{
    if (!m_ready) return;
    GhosttySelection selection = GHOSTTY_INIT_SIZED(GhosttySelection);
    if (ghostty_terminal_select_all(m_terminal, &selection) == GHOSTTY_SUCCESS) installSelection(&selection);
}

void GhosttyEngine::clearSelection()
{
    if (!m_ready) return;
    installSelection(nullptr);
    ghostty_selection_gesture_reset(m_gesture, m_terminal);
}

QString GhosttyEngine::selectedText() const
{
    if (!m_ready) return {};
    GhosttyTerminalSelectionFormatOptions options = GHOSTTY_INIT_SIZED(GhosttyTerminalSelectionFormatOptions);
#pragma push_macro("emit")
#undef emit
    options.emit = GHOSTTY_FORMATTER_FORMAT_PLAIN;
#pragma pop_macro("emit")
    options.unwrap = true;
    options.trim = true;
    options.selection = nullptr;
    uint8_t *text = nullptr;
    size_t length = 0;
    if (ghostty_terminal_selection_format_alloc(m_terminal, nullptr, options, &text, &length) != GHOSTTY_SUCCESS)
        return {};
    const QString result = QString::fromUtf8(reinterpret_cast<const char *>(text), qsizetype(length));
    ghostty_free(nullptr, text, length);
    return result;
}

void GhosttyEngine::setSearchNeedle(const QString &needle)
{
    if (!m_ready || needle == m_searchNeedle) return;
    m_searchNeedle = needle;
    if (!m_search) {
        if (needle.isEmpty()) return;
        if (ghostty_search_new(nullptr, &m_search, m_terminal) != GHOSTTY_SUCCESS) {
            m_search = nullptr;
            return;
        }
    }
    if (needle.isEmpty()) {
        ghostty_search_set(m_search, GHOSTTY_SEARCH_OPT_NEEDLE, nullptr);
        return;
    }
    const QByteArray utf8 = needle.toUtf8();
    const GhosttyString value{reinterpret_cast<const uint8_t *>(utf8.constData()), size_t(utf8.size())};
    ghostty_search_set(m_search, GHOSTTY_SEARCH_OPT_NEEDLE, &value);
}

void GhosttyEngine::selectSearchMatch(bool newer)
{
    if (!m_search || m_searchNeedle.isEmpty()) return;
    ghostty_search_set(m_search, newer ? GHOSTTY_SEARCH_OPT_SELECT_PREV : GHOSTTY_SEARCH_OPT_SELECT_NEXT, nullptr);
}

bool GhosttyEngine::advanceSearch()
{
    if (!m_search || m_searchNeedle.isEmpty()) return false;
    ghostty_search_feed(m_search);
    for (int step = 0; step < kSearchTicksPerStep; ++step) {
        GhosttySearchStatus status = GHOSTTY_SEARCH_STATUS_COMPLETE;
        if (ghostty_search_tick(m_search, &status) != GHOSTTY_SUCCESS) return false;
        if (status == GHOSTTY_SEARCH_STATUS_COMPLETE) return false;
        if (status == GHOSTTY_SEARCH_STATUS_FEED_REQUIRED) ghostty_search_feed(m_search);
    }
    return true;
}

TerminalSearchStatus GhosttyEngine::searchStatus() const
{
    TerminalSearchStatus status;
    status.needle = m_searchNeedle;
    if (!m_search || m_searchNeedle.isEmpty()) return status;
    size_t total = 0;
    ghostty_search_get(m_search, GHOSTTY_SEARCH_DATA_TOTAL_MATCHES, &total);
    status.total = int(std::min<size_t>(total, size_t(std::numeric_limits<int>::max())));
    size_t selected = 0;
    if (ghostty_search_get(m_search, GHOSTTY_SEARCH_DATA_SELECTED_INDEX, &selected) == GHOSTTY_SUCCESS)
        status.selected = int(std::min<size_t>(selected, size_t(std::numeric_limits<int>::max())));
    GhosttySearchStatus progress = GHOSTTY_SEARCH_STATUS_COMPLETE;
    ghostty_search_get(m_search, GHOSTTY_SEARCH_DATA_STATUS, &progress);
    status.complete = progress == GHOSTTY_SEARCH_STATUS_COMPLETE;
    return status;
}

bool GhosttyEngine::resolveColor(const GhosttyStyleColor &color, const GhosttyRenderStateColors &colors,
                                 QColor *resolved, int *paletteIndex) const
{
    *paletteIndex = -1;
    switch (color.tag) {
    case GHOSTTY_STYLE_COLOR_PALETTE: {
        const int index = color.value.palette;
        const GhosttyColorRgb rgb = colors.palette[index];
        *resolved = toColor(rgb);
        // A theme slot the child did not redefine (OSC 4) stays semantic, so
        // a theme switch recolours it.
        if (index < 16) {
            const GhosttyColorRgb base = basePaletteColor(index);
            if (rgb.r == base.r && rgb.g == base.g && rgb.b == base.b) *paletteIndex = index;
        }
        return true;
    }
    case GHOSTTY_STYLE_COLOR_RGB: *resolved = toColor(color.value.rgb); return true;
    default: return false;
    }
}

void GhosttyEngine::convertStyle(const GhosttyStyle &style, const GhosttyRenderStateColors &colors,
                                 CellStyle *cellStyle) const
{
    if (resolveColor(style.fg_color, colors, &cellStyle->foreground, &cellStyle->foregroundPaletteIndex)) {
        cellStyle->defaultForeground = false;
    } else if (m_defaultForeground.isValid() && !sameColor(colors.foreground, m_defaultForeground)) {
        // OSC 10 replaced the default: the child's colour, not the theme's.
        cellStyle->foreground = toColor(colors.foreground);
        cellStyle->defaultForeground = false;
    }
    if (resolveColor(style.bg_color, colors, &cellStyle->background, &cellStyle->backgroundPaletteIndex)) {
        cellStyle->defaultBackground = false;
    } else if (m_defaultBackground.isValid() && !sameColor(colors.background, m_defaultBackground)) {
        cellStyle->background = toColor(colors.background);
        cellStyle->defaultBackground = false;
    }
    resolveColor(style.underline_color, colors, &cellStyle->underlineColor, &cellStyle->underlineColorPaletteIndex);
    if (cellStyle->underlineColorPaletteIndex >= 0) cellStyle->underlineColor = QColor();
    cellStyle->bold = style.bold;
    cellStyle->faint = style.faint;
    cellStyle->italic = style.italic;
    cellStyle->underline = underlineStyle(style.underline);
    cellStyle->overline = style.overline;
    cellStyle->strike = style.strikethrough;
    cellStyle->blink = style.blink;
    cellStyle->reverse = style.inverse;
    cellStyle->conceal = style.invisible;
}

void GhosttyEngine::readCell(qint64 screenRow, int column, const GhosttyRenderStateColors &colors, Cell *cell) const
{
    GhosttyCell raw = 0;
    ghostty_render_state_row_cells_get(m_cells, GHOSTTY_RENDER_STATE_ROW_CELLS_DATA_RAW, &raw);
    GhosttyCellWide wide = GHOSTTY_CELL_WIDE_NARROW;
    GhosttyCellContentTag tag = GHOSTTY_CELL_CONTENT_CODEPOINT;
    bool hasText = false;
    bool hasStyling = false;
    bool hasHyperlink = false;
    const GhosttyCellData keys[] = {GHOSTTY_CELL_DATA_WIDE, GHOSTTY_CELL_DATA_CONTENT_TAG, GHOSTTY_CELL_DATA_HAS_TEXT,
                                    GHOSTTY_CELL_DATA_HAS_STYLING, GHOSTTY_CELL_DATA_HAS_HYPERLINK};
    void *values[] = {&wide, &tag, &hasText, &hasStyling, &hasHyperlink};
    ghostty_cell_get_multi(raw, std::size(keys), keys, values, nullptr);

    *cell = Cell{};
    if (wide == GHOSTTY_CELL_WIDE_WIDE) {
        cell->width = 2;
    } else if (wide == GHOSTTY_CELL_WIDE_SPACER_TAIL) {
        cell->width = 0;
        cell->continuation = true;
    }
    if (hasText) {
        uint32_t length = 0;
        ghostty_render_state_row_cells_get(m_cells, GHOSTTY_RENDER_STATE_ROW_CELLS_DATA_GRAPHEMES_LEN, &length);
        if (length > 0) {
            QVarLengthArray<uint32_t, 4> codepoints(qsizetype(length), 0u);
            ghostty_render_state_row_cells_get(m_cells, GHOSTTY_RENDER_STATE_ROW_CELLS_DATA_GRAPHEMES_BUF,
                                               codepoints.data());
            cell->text =
                QString::fromUcs4(reinterpret_cast<const char32_t *>(codepoints.constData()), qsizetype(length));
        }
    }
    if (hasStyling) {
        GhosttyStyle style = GHOSTTY_INIT_SIZED(GhosttyStyle);
        if (ghostty_render_state_row_cells_get(m_cells, GHOSTTY_RENDER_STATE_ROW_CELLS_DATA_STYLE, &style)
            == GHOSTTY_SUCCESS) {
            convertStyle(style, colors, &cell->style);
        }
    } else {
        convertStyle(GHOSTTY_INIT_SIZED(GhosttyStyle), colors, &cell->style);
    }
    // Background-only cells (erased with a coloured background) keep the
    // colour in the cell, not in a style.
    if (tag == GHOSTTY_CELL_CONTENT_BG_COLOR_PALETTE || tag == GHOSTTY_CELL_CONTENT_BG_COLOR_RGB) {
        GhosttyStyleColor background{};
        if (tag == GHOSTTY_CELL_CONTENT_BG_COLOR_PALETTE) {
            background.tag = GHOSTTY_STYLE_COLOR_PALETTE;
            ghostty_cell_get(raw, GHOSTTY_CELL_DATA_COLOR_PALETTE, &background.value.palette);
        } else {
            background.tag = GHOSTTY_STYLE_COLOR_RGB;
            ghostty_cell_get(raw, GHOSTTY_CELL_DATA_COLOR_RGB, &background.value.rgb);
        }
        if (resolveColor(background, colors, &cell->style.background, &cell->style.backgroundPaletteIndex))
            cell->style.defaultBackground = false;
    }
    if (hasHyperlink) {
        if (const std::optional<GhosttyGridRef> ref = screenRef(screenRow, column)) {
            QByteArray uri(256, Qt::Uninitialized);
            size_t length = 0;
            GhosttyResult result = ghostty_grid_ref_hyperlink_uri(&*ref, reinterpret_cast<uint8_t *>(uri.data()),
                                                                  size_t(uri.size()), &length);
            if (result == GHOSTTY_OUT_OF_SPACE) {
                uri.resize(qsizetype(length));
                result = ghostty_grid_ref_hyperlink_uri(&*ref, reinterpret_cast<uint8_t *>(uri.data()),
                                                        size_t(uri.size()), &length);
            }
            if (result == GHOSTTY_SUCCESS)
                cell->style.hyperlink = QString::fromUtf8(uri.constData(), qsizetype(length));
        }
    }
}

void GhosttyEngine::readLine(qint64 screenRow, const GhosttyRenderStateColors &colors, Line *line) const
{
    line->cells.resize(m_columns);
    int column = 0;
    bool spacerHead = false;
    while (column < m_columns && ghostty_render_state_row_cells_next(m_cells)) {
        readCell(screenRow, column, colors, &line->cells[column]);
        spacerHead = false;
        if (column == m_columns - 1) {
            GhosttyCell raw = 0;
            GhosttyCellWide wide = GHOSTTY_CELL_WIDE_NARROW;
            ghostty_render_state_row_cells_get(m_cells, GHOSTTY_RENDER_STATE_ROW_CELLS_DATA_RAW, &raw);
            ghostty_cell_get(raw, GHOSTTY_CELL_DATA_WIDE, &wide);
            spacerHead = wide == GHOSTTY_CELL_WIDE_SPACER_HEAD;
        }
        ++column;
    }
    GhosttyRow raw = 0;
    bool wrapped = false;
    if (ghostty_render_state_row_get(m_rowIterator, GHOSTTY_RENDER_STATE_ROW_DATA_RAW, &raw) == GHOSTTY_SUCCESS)
        ghostty_row_get(raw, GHOSTTY_ROW_DATA_WRAP, &wrapped);
    line->softWrapped = wrapped;
    // A wide glyph that did not fit wrapped early: its padding cell is
    // structure, not text.
    line->wrapColumns = wrapped && spacerHead ? m_columns - 1 : -1;
}

void GhosttyEngine::readCursor(Frame &frame) const
{
    GhosttyRenderStateCursor cursor = GHOSTTY_INIT_SIZED(GhosttyRenderStateCursor);
    if (ghostty_render_state_get(m_renderState, GHOSTTY_RENDER_STATE_DATA_CURSOR, &cursor) != GHOSTTY_SUCCESS) return;
    frame.cursor.row = cursor.viewport_y;
    frame.cursor.column = cursor.viewport_x;
    frame.cursor.visible = cursor.visible && cursor.viewport_has_value;
    frame.cursor.blink = cursor.blinking;
    switch (cursor.visual_style) {
    case GHOSTTY_RENDER_STATE_CURSOR_VISUAL_STYLE_BAR: frame.cursor.shape = CursorShape::Bar; break;
    case GHOSTTY_RENDER_STATE_CURSOR_VISUAL_STYLE_UNDERLINE: frame.cursor.shape = CursorShape::Underline; break;
    default: frame.cursor.shape = CursorShape::Block; break;
    }
}

void GhosttyEngine::readSearchMatches(Frame &frame, qint64 firstScreenRow) const
{
    if (!m_search || m_searchNeedle.isEmpty()) return;
    GhosttySelectionBuffer buffer{nullptr, 0, 0};
    if (ghostty_search_get(m_search, GHOSTTY_SEARCH_DATA_VIEWPORT_MATCHES, &buffer) == GHOSTTY_OUT_OF_SPACE) {
        QVector<GhosttySelection> matches(qsizetype(buffer.len), GHOSTTY_INIT_SIZED(GhosttySelection));
        buffer = {matches.data(), size_t(matches.size()), 0};
        if (ghostty_search_get(m_search, GHOSTTY_SEARCH_DATA_VIEWPORT_MATCHES, &buffer) != GHOSTTY_SUCCESS) return;
        matches.resize(qsizetype(buffer.len));

        std::optional<GhosttyPointCoordinate> currentStart;
        GhosttySelection current = GHOSTTY_INIT_SIZED(GhosttySelection);
        if (ghostty_search_get(m_search, GHOSTTY_SEARCH_DATA_SELECTED_MATCH, &current) == GHOSTTY_SUCCESS)
            currentStart = screenPoint(current.start);

        const qint64 lineCount = frame.lines.size();
        for (const GhosttySelection &match : std::as_const(matches)) {
            std::optional<GhosttyPointCoordinate> start = screenPoint(match.start);
            std::optional<GhosttyPointCoordinate> end = screenPoint(match.end);
            if (!start || !end) continue;
            if (std::make_pair(end->y, end->x) < std::make_pair(start->y, start->x)) std::swap(start, end);
            const bool isCurrent = currentStart && currentStart->x == start->x && currentStart->y == start->y;
            for (qint64 row = start->y; row <= qint64(end->y); ++row) {
                const qint64 line = row - firstScreenRow;
                if (line < 0 || line >= lineCount) continue;
                TerminalHighlight highlight;
                highlight.line = int(line);
                highlight.startColumn = row == qint64(start->y) ? start->x : 0;
                highlight.endColumn = row == qint64(end->y) ? end->x + 1 : m_columns;
                highlight.current = isCurrent;
                frame.searchMatches.push_back(highlight);
            }
        }
    }
}

void GhosttyEngine::readGraphics(Frame &frame)
{
    uint64_t generation = 0;
    ghostty_render_state_get(m_renderState, GHOSTTY_RENDER_STATE_DATA_IMAGE_PLACEMENTS_GENERATION, &generation);
    if (ghostty_render_state_get(m_renderState, GHOSTTY_RENDER_STATE_DATA_IMAGE_ITERATOR, &m_images)
        != GHOSTTY_SUCCESS) {
        return;
    }
    GhosttyKittyGraphics graphics = nullptr;
    ghostty_terminal_get(m_terminal, GHOSTTY_TERMINAL_DATA_KITTY_GRAPHICS, &graphics);

    bool resourcesChanged = false;
    QSet<QString> live;
    while (ghostty_render_state_image_iterator_next(m_images)) {
        GhosttyImageProtocol protocol = GHOSTTY_IMAGE_PROTOCOL_KITTY;
        GhosttyRenderStateKittyPlacementKind kind = GHOSTTY_RENDER_STATE_KITTY_PLACEMENT_KIND_VIRTUAL;
        ghostty_render_state_image_get(m_images, GHOSTTY_RENDER_STATE_IMAGE_DATA_PROTOCOL, &protocol);
        ghostty_render_state_image_get(m_images, GHOSTTY_RENDER_STATE_IMAGE_DATA_KITTY_KIND, &kind);
        // Virtual placements (Unicode placeholders) are not supported.
        if (protocol != GHOSTTY_IMAGE_PROTOCOL_KITTY || kind == GHOSTTY_RENDER_STATE_KITTY_PLACEMENT_KIND_VIRTUAL)
            continue;
        uint32_t imageId = 0, placementId = 0, gridColumns = 0, gridRows = 0, offsetX = 0, offsetY = 0;
        uint32_t pixelWidth = 0, pixelHeight = 0, sourceX = 0, sourceY = 0, sourceWidth = 0, sourceHeight = 0;
        uint64_t imageGeneration = 0;
        int32_t z = 0, column = 0, row = 0;
        const GhosttyRenderStateImageData keys[] = {
            GHOSTTY_RENDER_STATE_IMAGE_DATA_IMAGE_ID,         GHOSTTY_RENDER_STATE_IMAGE_DATA_KITTY_PLACEMENT_ID,
            GHOSTTY_RENDER_STATE_IMAGE_DATA_IMAGE_GENERATION, GHOSTTY_RENDER_STATE_IMAGE_DATA_Z,
            GHOSTTY_RENDER_STATE_IMAGE_DATA_VIEWPORT_COL,     GHOSTTY_RENDER_STATE_IMAGE_DATA_VIEWPORT_ROW,
            GHOSTTY_RENDER_STATE_IMAGE_DATA_GRID_COLS,        GHOSTTY_RENDER_STATE_IMAGE_DATA_GRID_ROWS,
            GHOSTTY_RENDER_STATE_IMAGE_DATA_CELL_OFFSET_X,    GHOSTTY_RENDER_STATE_IMAGE_DATA_CELL_OFFSET_Y,
            GHOSTTY_RENDER_STATE_IMAGE_DATA_PIXEL_WIDTH,      GHOSTTY_RENDER_STATE_IMAGE_DATA_PIXEL_HEIGHT,
            GHOSTTY_RENDER_STATE_IMAGE_DATA_SOURCE_X,         GHOSTTY_RENDER_STATE_IMAGE_DATA_SOURCE_Y,
            GHOSTTY_RENDER_STATE_IMAGE_DATA_SOURCE_WIDTH,     GHOSTTY_RENDER_STATE_IMAGE_DATA_SOURCE_HEIGHT,
        };
        void *values[] = {&imageId,     &placementId, &imageGeneration, &z,           &column,     &row,
                          &gridColumns, &gridRows,    &offsetX,         &offsetY,     &pixelWidth, &pixelHeight,
                          &sourceX,     &sourceY,     &sourceWidth,     &sourceHeight};
        if (ghostty_render_state_image_get_multi(m_images, std::size(keys), keys, values, nullptr) != GHOSTTY_SUCCESS)
            continue;

        const QString resourceId = QStringLiteral("%1:%2:%3,%4,%5x%6")
                                       .arg(imageId)
                                       .arg(imageGeneration)
                                       .arg(sourceX)
                                       .arg(sourceY)
                                       .arg(sourceWidth)
                                       .arg(sourceHeight);
        std::shared_ptr<const GraphicResource> resource = m_graphicResources.value(resourceId);
        if (!resource && graphics) {
            const GhosttyKittyGraphicsImage image = ghostty_kitty_graphics_image(graphics, imageId);
            QImage pixels = image ? kittyImage(image) : QImage();
            if (pixels.isNull()) continue;
            const QRect source{int(sourceX), int(sourceY), int(sourceWidth), int(sourceHeight)};
            if (!source.isEmpty() && source != pixels.rect()) pixels = pixels.copy(source);
            auto created = std::make_shared<GraphicResource>();
            created->id = resourceId;
            created->image = std::move(pixels);
            resource = created;
            m_graphicResources.insert(resourceId, resource);
            resourcesChanged = true;
        }
        if (!resource) continue;
        live.insert(resourceId);

        GraphicPlacement placement;
        placement.id = QStringLiteral("%1:%2").arg(imageId).arg(placementId);
        placement.resource = resource;
        placement.row = row;
        placement.column = column;
        placement.columns = int(gridColumns);
        placement.rows = int(gridRows);
        placement.cellOffset = QPoint(int(offsetX), int(offsetY));
        placement.pixelSize = QSize(int(pixelWidth), int(pixelHeight));
        placement.zIndex = z;
        frame.graphics.push_back(std::move(placement));
    }
    for (auto it = m_graphicResources.begin(); it != m_graphicResources.end();) {
        if (live.contains(it.key())) {
            ++it;
        } else {
            it = m_graphicResources.erase(it);
            resourcesChanged = true;
        }
    }
    if (resourcesChanged || generation != m_placementsGeneration) {
        m_placementsGeneration = generation;
        ++m_graphicsRevision;
    }
}

Frame GhosttyEngine::frame()
{
    Frame result;
    result.columns = m_columns;
    result.rows = m_rows;
    if (!m_ready) return result;

    ghostty_render_state_update(m_renderState, m_terminal);
    GhosttyRenderStateColors colors = GHOSTTY_INIT_SIZED(GhosttyRenderStateColors);
    ghostty_render_state_get(m_renderState, GHOSTTY_RENDER_STATE_DATA_COLORS, &colors);
    result.scrollbar = scrollbar();
    GhosttyTerminalScreen screen = GHOSTTY_TERMINAL_SCREEN_PRIMARY;
    ghostty_terminal_get(m_terminal, GHOSTTY_TERMINAL_DATA_ACTIVE_SCREEN, &screen);
    result.alternateScreen = screen == GHOSTTY_TERMINAL_SCREEN_ALTERNATE;
    result.promptMarks = m_promptMarks && !result.alternateScreen;

    GhosttyRenderStateDirty frameDirty = GHOSTTY_RENDER_STATE_DIRTY_FULL;
    ghostty_render_state_get(m_renderState, GHOSTTY_RENDER_STATE_DATA_DIRTY, &frameDirty);
    for (CachedLine &cached : m_lines) cached.seen = false;
    if (ghostty_render_state_get(m_renderState, GHOSTTY_RENDER_STATE_DATA_ROW_ITERATOR, &m_rowIterator)
        == GHOSTTY_SUCCESS) {
        result.lines.reserve(m_rows * 3);
        int rowsAbove = 0;
        while (ghostty_render_state_row_iterator_next(m_rowIterator)) {
            GhosttyRenderStateRowId rawId{};
            bool dirty = true;
            int32_t viewportY = 0;
            ghostty_render_state_row_get(m_rowIterator, GHOSTTY_RENDER_STATE_ROW_DATA_ID, &rawId);
            ghostty_render_state_row_get(m_rowIterator, GHOSTTY_RENDER_STATE_ROW_DATA_DIRTY, &dirty);
            ghostty_render_state_row_get(m_rowIterator, GHOSTTY_RENDER_STATE_ROW_DATA_VIEWPORT_Y, &viewportY);
            if (viewportY < 0) ++rowsAbove;
            const TerminalRowId id{rawId.bits[0], rawId.bits[1]};
            const qint64 screenRow = result.scrollbar.offset + viewportY;

            auto cached = m_lines.find(id);
            Line line;
            if (cached != m_lines.end() && !dirty && frameDirty != GHOSTTY_RENDER_STATE_DIRTY_FULL) {
                line = cached->line;
            } else {
                line.id = id;
                if (ghostty_render_state_row_get(m_rowIterator, GHOSTTY_RENDER_STATE_ROW_DATA_CELLS, &m_cells)
                    == GHOSTTY_SUCCESS) {
                    readLine(screenRow, colors, &line);
                } else {
                    line.cells.resize(m_columns);
                }
                // libghostty marks rows dirty generously; only a real change
                // costs the view a new prepared row.
                const bool same = cached != m_lines.end() && cached->line.cells == line.cells
                    && cached->line.softWrapped == line.softWrapped && cached->line.wrapColumns == line.wrapColumns;
                line.revision = same ? cached->line.revision : m_nextRevision++;
            }
            GhosttyRenderStateRowSelection selection = GHOSTTY_INIT_SIZED(GhosttyRenderStateRowSelection);
            if (ghostty_render_state_row_get(m_rowIterator, GHOSTTY_RENDER_STATE_ROW_DATA_SELECTION, &selection)
                == GHOSTTY_SUCCESS) {
                result.selection.push_back({int(result.lines.size()), selection.start_x, selection.end_x + 1, false});
            }
            m_lines.insert(id, CachedLine{line, true});
            result.lines.push_back(std::move(line));
        }
        result.viewportRow = rowsAbove;
    }
    m_lines.removeIf([](const QHash<TerminalRowId, CachedLine>::iterator it) { return !it->seen; });

    GhosttySelection active = GHOSTTY_INIT_SIZED(GhosttySelection);
    result.hasSelection = ghostty_terminal_get(m_terminal, GHOSTTY_TERMINAL_DATA_SELECTION, &active) == GHOSTTY_SUCCESS;
    readCursor(result);
    if (colors.cursor_has_value) result.cursor.color = toColor(colors.cursor);
    readSearchMatches(result, result.scrollbar.offset - result.viewportRow);
    readGraphics(result);
    ghostty_render_state_clean(m_renderState);
    return result;
}

} // namespace span::terminal
