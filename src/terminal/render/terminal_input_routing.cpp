#include "terminal/render/terminal_input_routing.h"

#include <QInputEvent>
#include <QKeySequence>

#include <cstdlib>

namespace span::terminal {

namespace {

// Win32 virtual keys and set-1 scan codes.
constexpr quint32 kLastVirtualKey = 0xFE;
constexpr quint32 kImeProcessKey = 0xE5;
constexpr quint32 kUnicodePacket = 0xE7;
constexpr quint32 kRightAltVirtualKey = 0xA5;
constexpr quint16 kVirtualKeyUp = 0x26;
constexpr quint16 kVirtualKeyDown = 0x28;
constexpr quint16 kScanCodeUp = 0x48;
constexpr quint16 kScanCodeDown = 0x50;

int unshiftedAsciiCodepoint(int codepoint)
{
    switch (codepoint) {
    case '~': return '`';
    case '!': return '1';
    case '@': return '2';
    case '#': return '3';
    case '$': return '4';
    case '%': return '5';
    case '^': return '6';
    case '&': return '7';
    case '*': return '8';
    case '(': return '9';
    case ')': return '0';
    case '_': return '-';
    case '+': return '=';
    case '{': return '[';
    case '}': return ']';
    case '|': return '\\';
    case ':': return ';';
    case '"': return '\'';
    case '<': return ',';
    case '>': return '.';
    case '?': return '/';
    default: return codepoint;
    }
}

bool functionalKey(int qtKey, TerminalKey *key, int *functionNumber)
{
    switch (qtKey) {
    case Qt::Key_Return:
    case Qt::Key_Enter: *key = TerminalKey::Enter; return true;
    case Qt::Key_Backtab:
    case Qt::Key_Tab: *key = TerminalKey::Tab; return true;
    case Qt::Key_Backspace: *key = TerminalKey::Backspace; return true;
    case Qt::Key_Escape: *key = TerminalKey::Escape; return true;
    case Qt::Key_Up: *key = TerminalKey::Up; return true;
    case Qt::Key_Down: *key = TerminalKey::Down; return true;
    case Qt::Key_Left: *key = TerminalKey::Left; return true;
    case Qt::Key_Right: *key = TerminalKey::Right; return true;
    case Qt::Key_Insert: *key = TerminalKey::Insert; return true;
    case Qt::Key_Delete: *key = TerminalKey::Delete; return true;
    case Qt::Key_Home: *key = TerminalKey::Home; return true;
    case Qt::Key_End: *key = TerminalKey::End; return true;
    case Qt::Key_PageUp: *key = TerminalKey::PageUp; return true;
    case Qt::Key_PageDown: *key = TerminalKey::PageDown; return true;
    default:
        if (qtKey >= Qt::Key_F1 && qtKey <= Qt::Key_F35) {
            *key = TerminalKey::Function;
            *functionNumber = qtKey - Qt::Key_F1 + 1;
            return true;
        }
        return false;
    }
}

// Windows delivers Ctrl/Alt chords with control characters or no text at all
// (menu mnemonics, dead keys). Recover the key's own character so the encoder
// sees one logical identity regardless of how Qt populated text().
QString shortcutText(const QKeyEvent *event)
{
    const int key = event->key();
    if (key >= Qt::Key_A && key <= Qt::Key_Z) {
        return QString(QChar(char16_t('a' + key - Qt::Key_A)));
    }
    if (key >= Qt::Key_0 && key <= Qt::Key_9) {
        return QString(QChar(char16_t('0' + key - Qt::Key_0)));
    }
    const QString text = event->text();
    const bool controlText = text.size() == 1
        && (text.front().unicode() < 0x20 || text.front().unicode() == 0x7f);
    if ((text.isEmpty() || controlText) && key >= 0x20 && key <= 0x7e) {
        return QString(QChar(char16_t(key)));
    }
    return text;
}

TerminalTextKey textKey(const QKeyEvent *event, const QString &text)
{
    TerminalTextKey result;
    const QList<uint> codepoints = text.toUcs4();
    const int textCodepoint = codepoints.size() == 1 ? int(codepoints.first()) : 0;
    result.codepoint = textCodepoint;

    if (event->key() >= Qt::Key_A && event->key() <= Qt::Key_Z) {
        result.codepoint = 'a' + event->key() - Qt::Key_A;
    } else if (event->key() >= Qt::Key_0 && event->key() <= Qt::Key_9) {
        result.codepoint = '0' + event->key() - Qt::Key_0;
    } else if (event->modifiers().testFlag(Qt::ShiftModifier) && textCodepoint > 0) {
        result.codepoint = unshiftedAsciiCodepoint(textCodepoint);
    }

    if (event->modifiers().testFlag(Qt::ShiftModifier)
        && textCodepoint > 0 && textCodepoint != result.codepoint) {
        result.shiftedCodepoint = textCodepoint;
    }

    const quint32 nativeKey = event->nativeVirtualKey();
    if (nativeKey >= 'A' && nativeKey <= 'Z') {
        result.baseLayoutCodepoint = int('a' + nativeKey - 'A');
    } else if (nativeKey >= '0' && nativeKey <= '9') {
        result.baseLayoutCodepoint = int(nativeKey);
    }
    return result;
}

// Qt's Windows platform plugin reports these in QKeyEvent::nativeModifiers()
// (GetKeyState at the time of the message, plus lParam's extended-key bit).
namespace QtWindowsNativeModifier {
constexpr quint32 ShiftLeft = 0x00000001;
constexpr quint32 ControlLeft = 0x00000002;
constexpr quint32 AltLeft = 0x00000004;
constexpr quint32 ShiftRight = 0x00000010;
constexpr quint32 ControlRight = 0x00000020;
constexpr quint32 AltRight = 0x00000040;
constexpr quint32 CapsLock = 0x00000100;
constexpr quint32 NumLock = 0x00000200;
constexpr quint32 ScrollLock = 0x00000400;
constexpr quint32 ExtendedKey = 0x01000000;
}

quint32 controlKeyState(const QKeyEvent *event)
{
    namespace Native = QtWindowsNativeModifier;
    namespace State = TerminalControlKeyState;
    const quint32 native = event->nativeModifiers();
    quint32 state = 0;
    if (native != 0) {
        // AltGr arrives as it does on the keyboard: left Ctrl plus right Alt.
        if (native & (Native::ShiftLeft | Native::ShiftRight)) state |= State::Shift;
        if (native & Native::ControlLeft) state |= State::LeftCtrl;
        if (native & Native::ControlRight) state |= State::RightCtrl;
        if (native & Native::AltLeft) state |= State::LeftAlt;
        if (native & Native::AltRight) state |= State::RightAlt;
        if (native & Native::CapsLock) state |= State::CapsLock;
        if (native & Native::NumLock) state |= State::NumLock;
        if (native & Native::ScrollLock) state |= State::ScrollLock;
        if (native & Native::ExtendedKey) state |= State::EnhancedKey;
        return state;
    }
    // Synthesized events carry no native state.
    const Qt::KeyboardModifiers modifiers = event->modifiers();
    if (modifiers.testFlag(Qt::ShiftModifier)) state |= State::Shift;
    if (modifiers.testFlag(Qt::ControlModifier)) state |= State::LeftCtrl;
    if (modifiers.testFlag(Qt::AltModifier)) state |= State::LeftAlt;
    return state;
}

// The record a Windows console program would read for this event. A key the
// IME consumed (VK_PROCESSKEY) or a character injected without a key
// (VK_PACKET) has no physical identity; its text is committed text.
TerminalNativeKey nativeKey(const QKeyEvent *event)
{
    TerminalNativeKey native;
    const quint32 virtualKey = event->nativeVirtualKey();
    if (virtualKey == 0 || virtualKey > kLastVirtualKey || virtualKey == kImeProcessKey
        || virtualKey == kUnicodePacket) {
        return native;
    }
    native.virtualKey = quint16(virtualKey);
    native.scanCode = quint16(event->nativeScanCode() & 0xFF);
    native.controlKeyState = controlKeyState(event);
    native.text = event->text();
    return native;
}

} // namespace

bool TerminalInputRouting::useLocalMouse(int mouseMode, const QInputEvent *event)
{
    return mouseMode == 0
        || (event && event->modifiers().testFlag(Qt::ShiftModifier));
}

QVector<TerminalKeyStroke> TerminalInputRouting::alternateScrollKeys(int rows)
{
    // Arrows are extended keys.
    const bool up = rows > 0;
    QVector<TerminalKeyStroke> strokes;
    strokes.reserve(std::abs(rows) * 2);
    for (int index = 0; index < std::abs(rows); ++index) {
        for (const TerminalKeyEvent event : {TerminalKeyEvent::Press, TerminalKeyEvent::Release}) {
            TerminalKeyStroke stroke = TerminalKeyStroke::named(
                up ? TerminalKey::Up : TerminalKey::Down, Qt::NoModifier, 0, event);
            stroke.native.virtualKey = up ? kVirtualKeyUp : kVirtualKeyDown;
            stroke.native.scanCode = up ? kScanCodeUp : kScanCodeDown;
            stroke.native.controlKeyState = TerminalControlKeyState::EnhancedKey;
            strokes.push_back(std::move(stroke));
        }
    }
    return strokes;
}

TerminalMouseButton TerminalInputRouting::mouseButton(Qt::MouseButton button)
{
    if (button == Qt::RightButton) {
        return TerminalMouseButton::Right;
    }
    if (button == Qt::MiddleButton) {
        return TerminalMouseButton::Middle;
    }
    return TerminalMouseButton::Left;
}

std::optional<TerminalKeyStroke> TerminalInputRouting::translateKey(const QKeyEvent *event,
                                                                   bool altGrActive,
                                                                   TerminalKeyEvent keyEvent)
{
    if (!event) {
        return std::nullopt;
    }
    TerminalKeyStroke result;
    result.event = keyEvent;
    result.native = nativeKey(event);
    result.modifiers = event->modifiers()
        & (Qt::ShiftModifier | Qt::ControlModifier | Qt::AltModifier | Qt::MetaModifier);
    TerminalKey key = TerminalKey::Enter;
    if (functionalKey(event->key(), &key, &result.functionNumber)) {
        if (event->key() == Qt::Key_Backtab) {
            result.modifiers |= Qt::ShiftModifier;
        }
        result.key = key;
        // Qt names only the keypad's Enter Key_Enter. Keypad navigation keys
        // (NumLock off) stay main-cluster keys.
        result.keypad = event->key() == Qt::Key_Enter;
        return result;
    }

    const bool control = result.modifiers.testFlag(Qt::ControlModifier);
    const bool alt = result.modifiers.testFlag(Qt::AltModifier);
    if (altGrActive && control && alt && !event->text().isEmpty()) {
        // Windows reports AltGr as Ctrl+Alt. The produced character already
        // carries the layout's meaning; encoding it as a Ctrl+Alt chord would
        // turn '@' or '{' on European layouts into a shortcut.
        result.text = event->text();
        result.modifiers &= ~(Qt::ControlModifier | Qt::AltModifier);
    } else if (control || alt) {
        result.text = shortcutText(event);
    } else if (!result.modifiers.testFlag(Qt::MetaModifier)) {
        // Plain and Shift-produced text, including punctuation delivered by
        // an IME as a key event. Win-key chords are left to the desktop.
        result.text = event->text();
    }
    if (result.text.isEmpty()) {
        return std::nullopt;
    }
    result.textKey = textKey(event, result.text);
    result.keypad = event->modifiers().testFlag(Qt::KeypadModifier);
    return result;
}

quint32 TerminalInputRouting::physicalKey(const QKeyEvent *event)
{
    if (!event) {
        return 0;
    }
    // Synthesized events carry no scan code; their Qt key is all there is.
    return event->nativeScanCode() != 0 ? event->nativeScanCode() : quint32(event->key());
}

bool TerminalInputRouting::identifiesAltGr(const QKeyEvent *event)
{
    if (!event) {
        return false;
    }
    return event->key() == Qt::Key_AltGr || event->nativeVirtualKey() == kRightAltVirtualKey
        || event->modifiers().testFlag(Qt::GroupSwitchModifier);
}

QHash<int, QStringList> TerminalInputRouting::shortcutTable(const QVariantMap &bindings)
{
    QHash<int, QStringList> table;
    for (auto it = bindings.constBegin(); it != bindings.constEnd(); ++it) {
        const QKeySequence sequence =
            QKeySequence::fromString(it.value().toString(), QKeySequence::PortableText);
        if (sequence.count() == 1) {
            table[sequence[0].toCombined()].append(it.key());
        }
    }
    return table;
}

QStringList TerminalInputRouting::shortcutActions(const QKeyEvent *event,
                                                  const QHash<int, QStringList> &table)
{
    if (!event || table.isEmpty()) {
        return {};
    }
    const Qt::KeyboardModifiers modifiers = event->modifiers()
        & (Qt::ControlModifier | Qt::ShiftModifier | Qt::AltModifier | Qt::MetaModifier);
    return table.value(QKeyCombination(modifiers, Qt::Key(event->key())).toCombined());
}

bool TerminalInputRouting::isPlainCtrlC(const QKeyEvent *event,
                                        Qt::KeyboardModifiers modifiers)
{
    return event && event->key() == Qt::Key_C
        && modifiers == Qt::ControlModifier;
}

} // namespace span::terminal
