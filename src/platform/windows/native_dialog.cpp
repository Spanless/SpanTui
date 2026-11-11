#include "platform/windows/native_dialog.h"

#include <QDir>

#include <windows.h>
#include <shellapi.h>

#include <string>

namespace span::platform::dialogs {
namespace {

// No owner window exists, so the box must not open behind whatever the
// user was looking at when the launch failed.
constexpr UINT kErrorStyle = MB_ICONERROR | MB_SETFOREGROUND | MB_TOPMOST;

int messageBox(const QString &title, const QString &text, UINT buttons)
{
    const std::wstring nativeTitle = title.toStdWString();
    const std::wstring nativeText = text.toStdWString();
    return MessageBoxW(nullptr, nativeText.c_str(), nativeTitle.c_str(),
                       kErrorStyle | buttons);
}

} // namespace

void showError(const QString &title, const QString &text)
{
    messageBox(title, text, MB_OK);
}

bool askError(const QString &title, const QString &text)
{
    return messageBox(title, text, MB_YESNO) == IDYES;
}

void revealFolder(const QString &directory)
{
    const std::wstring path = QDir::toNativeSeparators(directory).toStdWString();
    ShellExecuteW(nullptr, L"explore", path.c_str(), nullptr, nullptr,
                  SW_SHOWNORMAL);
}

} // namespace span::platform::dialogs
