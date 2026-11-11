#include <QBuffer>
#include <QCoreApplication>
#include <QDataStream>
#include <QDebug>
#include <QFile>
#include <QImage>
#include <QPainter>
#include <QSvgRenderer>

#include <array>

namespace {

struct IconFrame {
    int size = 0;
    QByteArray payload;
};

QByteArray encodeWindowsDib(const QImage &image)
{
    QByteArray dib;
    QBuffer output(&dib);
    output.open(QIODevice::WriteOnly);
    QDataStream stream(&output);
    stream.setByteOrder(QDataStream::LittleEndian);

    const qint32 width = image.width();
    const qint32 height = image.height();
    const quint32 pixelBytes = quint32(width * height * 4);
    stream << quint32(40)             // BITMAPINFOHEADER size
           << width
           << qint32(height * 2)      // XOR bitmap plus AND mask
           << quint16(1)              // planes
           << quint16(32)             // bits per pixel
           << quint32(0)              // BI_RGB
           << pixelBytes
           << qint32(0) << qint32(0)  // pixels per meter
           << quint32(0) << quint32(0);

    // ICO DIB scanlines are bottom-up. QImage::Format_ARGB32 is BGRA in
    // little-endian memory, which is the byte layout expected by a 32-bit DIB.
    for (int y = height - 1; y >= 0; --y) {
        stream.writeRawData(reinterpret_cast<const char *>(image.constScanLine(y)), width * 4);
    }

    // Retain a zeroed, DWORD-aligned 1-bit mask for older Shell consumers.
    // Alpha remains authoritative for all rendered pixels.
    const int maskStride = ((width + 31) / 32) * 4;
    dib.append(QByteArray(maskStride * height, '\0'));
    return dib;
}

QByteArray renderIcon(const QString &svgPath, QString *error)
{
    QSvgRenderer renderer(svgPath);
    if (!renderer.isValid()) {
        *error = QStringLiteral("Invalid SVG: %1").arg(svgPath);
        return {};
    }

    constexpr std::array<int, 7> sizes{16, 24, 32, 48, 64, 128, 256};
    std::array<IconFrame, sizes.size()> frames;
    for (qsizetype index = 0; index < static_cast<qsizetype>(sizes.size()); ++index) {
        const int size = sizes[static_cast<size_t>(index)];
        QImage image(size, size, QImage::Format_ARGB32);
        image.fill(Qt::transparent);
        QPainter painter(&image);
        painter.setRenderHint(QPainter::Antialiasing, true);
        painter.setRenderHint(QPainter::SmoothPixmapTransform, true);
        renderer.render(&painter, QRectF(0, 0, size, size));
        painter.end();

        QByteArray payload;
        if (size == 256) {
            QBuffer buffer(&payload);
            if (!buffer.open(QIODevice::WriteOnly) || !image.save(&buffer, "PNG")) {
                *error = QStringLiteral("Could not encode the %1 px icon frame").arg(size);
                return {};
            }
        } else {
            payload = encodeWindowsDib(image);
        }
        frames[static_cast<size_t>(index)] = IconFrame{size, payload};
    }

    QByteArray ico;
    QBuffer output(&ico);
    output.open(QIODevice::WriteOnly);
    QDataStream stream(&output);
    stream.setByteOrder(QDataStream::LittleEndian);
    stream << quint16(0) << quint16(1) << quint16(frames.size());

    quint32 offset = 6 + static_cast<quint32>(frames.size()) * 16;
    for (const IconFrame &frame : frames) {
        stream << quint8(frame.size == 256 ? 0 : frame.size)
               << quint8(frame.size == 256 ? 0 : frame.size)
               << quint8(0) << quint8(0)
               << quint16(1) << quint16(32)
               << quint32(frame.payload.size()) << offset;
        offset += static_cast<quint32>(frame.payload.size());
    }
    for (const IconFrame &frame : frames)
        stream.writeRawData(frame.payload.constData(), frame.payload.size());
    return ico;
}

} // namespace

int main(int argc, char *argv[])
{
    QCoreApplication app(argc, argv);
    if (app.arguments().size() != 3) {
        qCritical("Usage: spantui_brand_icon_generator <input.svg> <output.ico>");
        return 2;
    }

    const QString inputPath = app.arguments().at(1);
    const QString outputPath = app.arguments().at(2);
    QString error;
    const QByteArray generated = renderIcon(inputPath, &error);
    if (generated.isEmpty()) {
        qCritical().noquote() << error;
        return 1;
    }

    QFile existing(outputPath);
    if (existing.open(QIODevice::ReadOnly) && existing.readAll() == generated)
        return 0;

    QFile output(outputPath);
    if (!output.open(QIODevice::WriteOnly | QIODevice::Truncate)
        || output.write(generated) != generated.size()
        || !output.flush()) {
        qCritical().noquote() << QStringLiteral("Could not write ICO: %1").arg(outputPath);
        return 1;
    }
    return 0;
}
