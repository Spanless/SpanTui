#pragma once

namespace span::platform {

// Whether the keyboard has a Command key the shell never receives (macOS).
// There Qt's portable "Ctrl" is Command: application and clipboard chords
// take it, and the application menu lives in the system menu bar, whose key
// equivalents run those chords. Elsewhere plain Ctrl chords belong to the
// shell, so application chords add Shift.
bool hasCommandKey();

} // namespace span::platform
