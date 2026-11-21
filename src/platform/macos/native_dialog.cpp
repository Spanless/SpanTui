#include "platform/native_dialog.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>

#include <CoreFoundation/CoreFoundation.h>
#include <CoreServices/CoreServices.h>

namespace span::platform::dialogs {

namespace {

// A Core Foundation alert: no window, event loop or Qt GUI needed, so it also
// works when the QML window never appeared. True when the default (first)
// button was chosen.
bool alert(const QString &title, const QString &text, const QString &defaultButton, const QString &otherButton)
{
    CFStringRef cfTitle = title.toCFString();
    CFStringRef cfText = text.toCFString();
    CFStringRef cfDefault = defaultButton.isEmpty() ? nullptr : defaultButton.toCFString();
    CFStringRef cfOther = otherButton.isEmpty() ? nullptr : otherButton.toCFString();
    CFOptionFlags response = kCFUserNotificationCancelResponse;
    const SInt32 result =
        CFUserNotificationDisplayAlert(0, kCFUserNotificationStopAlertLevel, nullptr, nullptr, nullptr, cfTitle, cfText,
                                       cfDefault, cfOther, nullptr, &response);
    for (CFStringRef value : {cfTitle, cfText, cfDefault, cfOther}) {
        if (value) CFRelease(value);
    }
    return result == 0 && response == kCFUserNotificationDefaultResponse;
}

} // namespace

void showError(const QString &title, const QString &text)
{
    alert(title, text, {}, {});
}

bool askError(const QString &title, const QString &text)
{
    return alert(title, text, QCoreApplication::translate("NativeDialog", "Yes"),
                 QCoreApplication::translate("NativeDialog", "No"));
}

void revealFolder(const QString &directory)
{
    const QByteArray path = QFile::encodeName(QDir(directory).absolutePath());
    CFURLRef url = CFURLCreateFromFileSystemRepresentation(
        kCFAllocatorDefault, reinterpret_cast<const UInt8 *>(path.constData()), CFIndex(path.size()), true);
    if (!url) return;
    LSOpenCFURLRef(url, nullptr);
    CFRelease(url);
}

} // namespace span::platform::dialogs
