#pragma once

#include <QLatin1StringView>
#include <QtGlobal>

namespace span::terminal {

// Keys of the options map of TerminalManager::createSession and
// TerminalSession::setLaunchOptions.
inline constexpr QLatin1StringView kSessionCommandLineKey{"commandLine"};
inline constexpr QLatin1StringView kSessionEnvironmentKey{"environment"};
inline constexpr QLatin1StringView kSessionColumnsKey{"columns"};
inline constexpr QLatin1StringView kSessionRowsKey{"rows"};
inline constexpr QLatin1StringView kSessionMaxScrollbackKey{"maxScrollback"};

// Grid size before the first resize.
inline constexpr int kDefaultColumns = 120;
inline constexpr int kDefaultRows = 32;

// TerminalView::cursorStyle values.
inline constexpr QLatin1StringView kCursorStyleBlock{"block"};
inline constexpr QLatin1StringView kCursorStyleBar{"bar"};
inline constexpr QLatin1StringView kCursorStyleUnderline{"underline"};

// Shortcut action ids that TerminalView handles itself; other bound actions
// leave through shortcutActivated.
inline constexpr QLatin1StringView kActionCopy{"terminal.copy"};
inline constexpr QLatin1StringView kActionPaste{"terminal.paste"};
inline constexpr QLatin1StringView kActionPreviousPrompt{"terminal.previousPrompt"};
inline constexpr QLatin1StringView kActionNextPrompt{"terminal.nextPrompt"};

// TerminalView::maximumFrameRate.
inline constexpr int kDefaultFrameRate = 60;
inline constexpr int kMinimumFrameRate = 30;
inline constexpr int kMaximumFrameRate = 360;

// Built-in schemes used when a stored scheme name no longer exists.
inline constexpr QLatin1StringView kDefaultDarkColorScheme{"Span Dark"};
inline constexpr QLatin1StringView kDefaultLightColorScheme{"Span Light"};

// Large but finite history, in rows. -1 lifts the row limit (tests); the
// engine's byte budget still applies. libghostty prunes whole pages, so
// somewhat more rows survive.
inline constexpr int kDefaultMaxScrollbackLines = 100'000;

// Floor for font-shaped text against the cell that actually sits behind it.
// Applies to default, ANSI and truecolor alike: a light-theme "dim" slot on a
// dark composer bar is as unreadable as a dark-theme gray on the light plane.
// Box/block/braille chrome is not lifted.
inline constexpr qreal kMinimumReadableContrast = 4.5;

inline constexpr qsizetype kOutputQueueMaximumBytes = 1024 * 1024;

// TerminalPresentationScheduler.
// A write that has not reached its end is held this long before a forced
// (possibly torn) cut, so a saturating producer still presents.
inline constexpr qint64 kMidWriteHoldMs = 50;
// A DEC 2026 transaction whose bytes stopped arriving is force-released.
inline constexpr qint64 kSynchronizedStallMs = 250;
// ED3 (CSI 3 J) opens an implicit redraw: released after the queue has
// drained and stayed quiet, or after the bounded hold.
inline constexpr qint64 kImplicitRedrawQuietMs = 50;
inline constexpr qint64 kImplicitRedrawMaxHoldMs = 1000;
// After a resize the child repaints; output-driven frames in this window
// wait for a quiet gap, each burst hidden for at most kRepaintMaxHoldMs.
inline constexpr qint64 kRepaintSettleWindowMs = 750;
inline constexpr qint64 kRepaintQuietMs = 50;
inline constexpr qint64 kRepaintMaxHoldMs = 350;

// TerminalCursorAnimation.
inline constexpr qint64 kCursorBlinkCycleMs = 1060;
// 30 Hz caret fade. Idle GPU cost is paid by stopping after the hold, not by
// matching the display refresh.
inline constexpr int kCursorSmoothTickMs = 33;
inline constexpr int kCursorDiscreteTickMs = 530;
inline constexpr qint64 kCursorBlinkHoldMs = 5000;
// Fully off at the trough so blink does not leave a dim ghost.
inline constexpr qreal kCursorMinimumOpacity = 0.0;

// TerminalScrollModel.
// Animated scrolling closes this share of the remaining distance per time
// constant (exponential ease-out).
inline constexpr double kScrollTimeConstantMs = 40.0;
// Closer than this to the target, the animation snaps and ends.
inline constexpr double kScrollSettleLines = 0.01;
// A frame advances the animation by at most this much time, so a stalled
// frame resumes the ease instead of landing on the target in one step.
inline constexpr qint64 kScrollMaxFrameMs = 33;

} // namespace span::terminal
