#pragma once

#include <QQuickImageProvider>

// Icons embedded in programs and .ico files, for QML under
// kProgramIconProviderId. The first icon of the file at the requested size;
// a null image when it has none.
class ProgramIconProvider final : public QQuickImageProvider {
public:
    ProgramIconProvider();

    QImage requestImage(const QString &id, QSize *size, const QSize &requestedSize) override;
};
