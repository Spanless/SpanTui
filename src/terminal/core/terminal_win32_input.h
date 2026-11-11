#pragma once

#include "terminal/api/terminal_types.h"

#include <QByteArray>
#include <QString>

namespace span::terminal {

// win32-input-mode (DECSET 9001, set by conhost at every ConPTY start): keys
// travel as KEY_EVENT_RECORDs and conhost translates them for VT children.
// libghostty lacks the mode; GhosttyEngine uses these while it is on and no
// VT keyboard protocol (Kitty, modifyOtherKeys) was negotiated.
namespace TerminalWin32Input {

// The key's records, or empty when the stroke has no physical key behind it
// (an IME placeholder or a synthetic event).
QByteArray keyRecords(const TerminalKeyStroke &stroke);
// Text no key produced: IME commits, programmatic input and pasted bodies.
// conhost gives ASCII its layout's own virtual key; anything else written as
// UTF-8 that fits its ANSI code page (“ ” ‘ ’ …) becomes an Alt+numpad
// sequence whose character rides only on the Alt key-up, which
// crossterm-based programs drop. Each unit therefore travels as a character
// record pair.
QByteArray text(const QString &text);

} // namespace TerminalWin32Input

} // namespace span::terminal
