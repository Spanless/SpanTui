#include "terminal/core/terminal_win32_input.h"

namespace span::terminal {
namespace {

// One KEY_EVENT_RECORD in win32-input-mode: CSI Vk;Sc;Uc;Kd;Cs;Rc _
QByteArray win32Record(quint16 virtualKey, quint16 scanCode, char16_t unit, bool keyDown, quint32 controlKeyState)
{
    return QByteArrayLiteral("\x1b[") + QByteArray::number(virtualKey) + ';' + QByteArray::number(scanCode) + ';'
        + QByteArray::number(int(unit)) + ';' + (keyDown ? '1' : '0') + ';' + QByteArray::number(controlKeyState)
        + ";1_";
}

// A character no key produced: a record pair without a virtual key, which
// the console delivers as the character itself.
QByteArray win32Character(char16_t unit)
{
    return win32Record(0, 0, unit, true, 0) + win32Record(0, 0, unit, false, 0);
}

} // namespace

QByteArray TerminalWin32Input::keyRecords(const TerminalKeyStroke &stroke)
{
    const TerminalNativeKey &native = stroke.native;
    if (native.virtualKey == 0) return {};
    const bool keyDown = stroke.event != TerminalKeyEvent::Release;
    // A key that produced one UTF-16 unit carries it in its own record, as
    // the console does. Longer output (a surrogate pair, a dead key's accent
    // plus the letter) follows as character records on key down.
    const char16_t unit = native.text.size() == 1 ? native.text.front().unicode() : 0;
    QByteArray bytes = win32Record(native.virtualKey, native.scanCode, unit, keyDown, native.controlKeyState);
    if (keyDown && native.text.size() > 1) {
        for (QChar extra : native.text) bytes += win32Character(extra.unicode());
    }
    return bytes;
}

QByteArray TerminalWin32Input::text(const QString &text)
{
    QByteArray bytes;
    for (QChar unit : text) {
        if (unit.unicode() < 0x80)
            bytes += char(unit.unicode());
        else
            bytes += win32Character(unit.unicode());
    }
    return bytes;
}

} // namespace span::terminal
