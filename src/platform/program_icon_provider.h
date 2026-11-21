#pragma once

#include <QQuickImageProvider>

// The icon the system shows for a program (an .exe or .ico file on Windows,
// an .app bundle on macOS), for QML under kProgramIconProviderId, at the
// requested size; a null image when the file has none.
class ProgramIconProvider final : public QQuickImageProvider {
public:
    ProgramIconProvider();

    QImage requestImage(const QString &id, QSize *size, const QSize &requestedSize) override;
};
