#include "application/preferences.h"

namespace span::application {

bool Preferences::open(SettingsStore::Mode mode)
{
    if (!store.open(mode)) {
        return false;
    }
    appearance.load();
    terminal.load();
    workspace.load();
    return true;
}

} // namespace span::application
