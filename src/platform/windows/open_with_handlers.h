#pragma once

#include <QList>
#include <QString>
#include <QStringList>

namespace span::platform {

// A desktop program Windows offers in "Open with" for a file type.
struct OpenWithHandler {
    // Name as the "Open with" menu shows it.
    QString name;
    // Absolute path of the program.
    QString executable;
};

// Programs Windows recommends in "Open with" for any of `extensions`
// (".md", …) — the list Explorer itself shows, from SHAssocEnumHandlers,
// so nothing here names a particular editor. Kept: installed desktop
// programs (an existing .exe) outside the Windows directory, other than
// `excludeExecutable`; packaged apps cannot take a folder argument and are
// left out. One entry per program, ordered by name. Blocking (registry and
// COM reads): call it off the GUI thread; it initialises COM itself.
QList<OpenWithHandler> recommendedOpenWithHandlers(const QStringList &extensions, const QString &excludeExecutable);

// Explorer's own executable, for its icon.
QString fileExplorerExecutable();

} // namespace span::platform
