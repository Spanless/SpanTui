#include "platform/windows/open_with_handlers.h"

#include <QDir>
#include <QFileInfo>
#include <QSet>

#include <algorithm>

#include <windows.h>
#include <objbase.h>
#include <shobjidl.h>

namespace span::platform {

namespace {

QString takeCoString(LPWSTR value)
{
    if (!value) {
        return {};
    }
    const QString text = QString::fromWCharArray(value);
    CoTaskMemFree(value);
    return text;
}

QString windowsDirectory()
{
    wchar_t buffer[MAX_PATH] = {};
    const UINT length = GetWindowsDirectoryW(buffer, MAX_PATH);
    return length > 0 && length < MAX_PATH ? QDir::cleanPath(QString::fromWCharArray(buffer, int(length))) : QString();
}

bool insideDirectory(const QString &path, const QString &directory)
{
    return !directory.isEmpty() && path.startsWith(directory + QLatin1Char('/'), Qt::CaseInsensitive);
}

} // namespace

QList<OpenWithHandler> recommendedOpenWithHandlers(const QStringList &extensions, const QString &excludeExecutable)
{
    const HRESULT init = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    const QString windows = windowsDirectory();
    const QString excluded = QDir::cleanPath(QDir::fromNativeSeparators(excludeExecutable));
    QList<OpenWithHandler> handlers;
    QSet<QString> seen;
    for (const QString &extension : extensions) {
        IEnumAssocHandlers *enumerator = nullptr;
        if (FAILED(SHAssocEnumHandlers(reinterpret_cast<LPCWSTR>(extension.utf16()), ASSOC_FILTER_RECOMMENDED,
                                       &enumerator))
            || !enumerator) {
            continue;
        }
        IAssocHandler *handler = nullptr;
        ULONG fetched = 0;
        while (enumerator->Next(1, &handler, &fetched) == S_OK && fetched == 1) {
            LPWSTR rawPath = nullptr;
            LPWSTR rawName = nullptr;
            const QString path = SUCCEEDED(handler->GetName(&rawPath)) ? takeCoString(rawPath) : QString();
            const QString name = SUCCEEDED(handler->GetUIName(&rawName)) ? takeCoString(rawName) : QString();
            handler->Release();
            handler = nullptr;

            // Packaged apps report an app id, not a program path.
            const QFileInfo info(path);
            if (!info.isAbsolute() || !info.isFile()
                || info.suffix().compare(QLatin1String("exe"), Qt::CaseInsensitive) != 0) {
                continue;
            }
            const QString executable = QDir::cleanPath(info.absoluteFilePath());
            const QString key = executable.toLower();
            if (seen.contains(key) || insideDirectory(executable, windows)
                || (!excluded.isEmpty() && executable.compare(excluded, Qt::CaseInsensitive) == 0)) {
                continue;
            }
            seen.insert(key);
            handlers.append({name.isEmpty() ? info.completeBaseName() : name, executable});
        }
        enumerator->Release();
    }
    if (SUCCEEDED(init)) {
        CoUninitialize();
    }
    std::sort(handlers.begin(), handlers.end(), [](const OpenWithHandler &a, const OpenWithHandler &b) {
        return a.name.compare(b.name, Qt::CaseInsensitive) < 0;
    });
    return handlers;
}

QString fileExplorerExecutable()
{
    const QString windows = windowsDirectory();
    return windows.isEmpty() ? QString() : windows + QStringLiteral("/explorer.exe");
}

} // namespace span::platform
