#pragma once

#include <QList>
#include <QString>
#include <QStringList>

namespace span::platform {

// A desktop program the system offers in "Open with" for a file type.
struct OpenWithHandler {
    // Name as the "Open with" menu shows it.
    QString name;
    // Absolute path of the program.
    QString executable;
};

// Programs the system recommends in "Open with" for any of `extensions`
// (".md", …), so nothing here names a particular editor. Other than
// `excludeExecutable`; one entry per program, ordered by name. Blocking:
// call it off the GUI thread.
//   Windows: the list Explorer shows (SHAssocEnumHandlers), keeping installed
//   desktop programs (an existing .exe) outside the Windows directory;
//   packaged apps cannot take a folder argument and are left out. Initialises
//   COM itself.
//   macOS: the applications Launch Services can open those content types
//   with; `executable` is the .app bundle.
QList<OpenWithHandler> recommendedOpenWithHandlers(const QStringList &extensions, const QString &excludeExecutable);

// The system file manager (Explorer, Finder), for its icon.
QString fileExplorerExecutable();

// Opens `folder` in a program from recommendedOpenWithHandlers(). The program
// outlives SpanTui: it is the user's editor, not a helper.
bool openFolderWith(const QString &executable, const QString &folder, QString *error);

} // namespace span::platform
