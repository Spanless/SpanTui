#pragma once

#include "application/appearance_settings.h"
#include "application/clipboard.h"
#include "application/settings_store.h"
#include "application/terminal_settings.h"
#include "application/workspace_settings.h"

namespace span::application {

// The preference objects of one process over one settings store. Hosts open()
// it (the gallery does not and serves defaults) and expose it to QML
// (runtime/qml_types.h) before creating their QML engine; it must outlive
// that engine.
struct Preferences final {
    Preferences() = default;
    Preferences(const Preferences &) = delete;
    Preferences &operator=(const Preferences &) = delete;

    // Opens the store, then loads every preference object from it.
    bool open(SettingsStore::Mode mode);

    SettingsStore store;
    AppearanceSettings appearance{&store};
    TerminalSettings terminal{&store, &appearance};
    WorkspaceSettings workspace{&store};
    Clipboard clipboard;
};

} // namespace span::application
