#pragma once

#include "terminal/api/terminal_types.h"

#include <QHash>
#include <QKeyEvent>
#include <QString>
#include <QStringList>
#include <QVariantMap>

#include <optional>

class QInputEvent;

namespace span::terminal {

class TerminalInputRouting final {
public:
    static bool useLocalMouse(int mouseMode, const QInputEvent *event);
    static TerminalMouseButton mouseButton(Qt::MouseButton button);
    // Press and release share the translation, so a release names the key its
    // press reported. Empty for modifier-only keys and Win chords.
    static std::optional<TerminalKeyStroke> translateKey(const QKeyEvent *event,
                                                         bool altGrActive,
                                                         TerminalKeyEvent keyEvent);
    // The physical key behind an event, equal for its press and release even
    // when modifiers change in between (Shift+2 presses '@', releases '2').
    static quint32 physicalKey(const QKeyEvent *event);
    static bool identifiesAltGr(const QKeyEvent *event);
    // Keyed by QKeyCombination::toCombined(); a chord may carry several
    // actions (id order). Unparsable and multi-chord sequences are dropped.
    static QHash<int, QStringList> shortcutTable(const QVariantMap &bindings);
    // Actions bound to this press, possibly none. Callers skip the lookup
    // while AltGr or an IME composition owns the key.
    static QStringList shortcutActions(const QKeyEvent *event,
                                       const QHash<int, QStringList> &table);
    static bool isPlainCtrlC(const QKeyEvent *event, Qt::KeyboardModifiers modifiers);
    // Whether a focused terminal claims this press (QEvent::ShortcutOverride)
    // ahead of application shortcuts and, on macOS, the menu bar's key
    // equivalents. A chord bound to a terminal action is claimed. Otherwise
    // every press is, except an application chord (`applicationTable`, the
    // window's own commands) and an unbound Super chord (Command, Windows
    // key): those never reach the shell (translateKey), so claiming them
    // would only take ⌘Q, ⌘H, ⌘M and the like from the system.
    static bool claimsShortcutOverride(const QKeyEvent *event, const QHash<int, QStringList> &table,
                                       const QHash<int, QStringList> &applicationTable = {});
    // Alternate scroll mode: wheel rows on an alternate screen without mouse
    // reporting become Up (rows > 0) or Down presses and releases, carrying
    // the arrow keys' own records so console programs see real keys.
    static QVector<TerminalKeyStroke> alternateScrollKeys(int rows);
};

} // namespace span::terminal
