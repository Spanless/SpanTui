#include "platform/windows/program_icon_provider.h"

#include <QDir>
#include <QUrl>

#include <windows.h>

ProgramIconProvider::ProgramIconProvider() : QQuickImageProvider(QQuickImageProvider::Image) {}

QImage ProgramIconProvider::requestImage(const QString &id, QSize *size,
                                         const QSize &requestedSize)
{
    const QString path = QDir::toNativeSeparators(QUrl::fromPercentEncoding(id.toUtf8()));
    const int edge = qBound(16, qMax(requestedSize.width(), requestedSize.height()), 256);
    QImage image;
    HICON icon = nullptr;
    // Unlike ExtractIconEx, picks the image closest to the requested size
    // instead of the 32 px or 16 px one scaled.
    if (!path.isEmpty()
        && PrivateExtractIconsW(reinterpret_cast<LPCWSTR>(path.utf16()), 0, edge, edge, &icon,
                                nullptr, 1, 0) == 1
        && icon) {
        image = QImage::fromHICON(icon);
        DestroyIcon(icon);
    }
    if (size) {
        *size = image.size();
    }
    return image;
}
