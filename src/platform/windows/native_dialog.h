#pragma once

#include <QString>

// Operating-system message boxes that need no Qt window, scene graph or event
// loop: they are the only way to show a failure that happened before (or
// because) the QML window could not appear. Blocking; callable from any
// thread.
namespace span::platform::dialogs {

void showError(const QString &title, const QString &text);

// Error box with Yes/No; true when the user chose Yes.
bool askError(const QString &title, const QString &text);

// Opens the directory in the system file manager.
void revealFolder(const QString &directory);

} // namespace span::platform::dialogs
