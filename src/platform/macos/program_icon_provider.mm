#include "platform/program_icon_provider.h"

#include <QUrl>

#import <AppKit/AppKit.h>

namespace {
// Icons come in sizes up to 1024 px; the menus ask for far less.
constexpr int kMinEdge = 16;
constexpr int kMaxEdge = 256;
} // namespace

ProgramIconProvider::ProgramIconProvider() : QQuickImageProvider(QQuickImageProvider::Image) {}

QImage ProgramIconProvider::requestImage(const QString &id, QSize *size, const QSize &requestedSize)
{
    const QString path = QUrl::fromPercentEncoding(id.toUtf8());
    const int edge = qBound(kMinEdge, qMax(requestedSize.width(), requestedSize.height()), kMaxEdge);
    QImage image;
    @autoreleasepool {
        NSImage *icon = path.isEmpty() ? nil : [[NSWorkspace sharedWorkspace] iconForFile:path.toNSString()];
        NSRect rect = NSMakeRect(0, 0, edge, edge);
        // Picks the representation closest to the requested size instead of
        // scaling the default one.
        CGImageRef cgImage = icon ? [icon CGImageForProposedRect:&rect context:nil hints:nil] : nullptr;
        if (cgImage) {
            image = QImage(edge, edge, QImage::Format_ARGB32_Premultiplied);
            image.fill(Qt::transparent);
            CGColorSpaceRef space = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
            CGContextRef context = CGBitmapContextCreate(image.bits(), size_t(edge), size_t(edge), 8,
                                                         size_t(image.bytesPerLine()), space,
                                                         kCGImageAlphaPremultipliedFirst | kCGBitmapByteOrder32Host);
            CGColorSpaceRelease(space);
            if (context) {
                CGContextSetInterpolationQuality(context, kCGInterpolationHigh);
                CGContextDrawImage(context, CGRectMake(0, 0, edge, edge), cgImage);
                CGContextRelease(context);
            } else {
                image = QImage();
            }
        }
    }
    if (size) *size = image.size();
    return image;
}
