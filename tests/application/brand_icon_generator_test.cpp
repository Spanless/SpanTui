#include <QDataStream>
#include <QFile>
#include <QProcess>
#include <QTemporaryDir>
#include <QTest>

namespace {
struct IconEntry {
    int width = 0;
    int height = 0;
    quint16 planes = 0;
    quint16 bitsPerPixel = 0;
    quint32 payloadSize = 0;
    quint32 payloadOffset = 0;
};

IconEntry readEntry(QDataStream &stream)
{
    quint8 width = 0;
    quint8 height = 0;
    quint8 colorCount = 0;
    quint8 reserved = 0;
    IconEntry entry;
    stream >> width >> height >> colorCount >> reserved
           >> entry.planes >> entry.bitsPerPixel
           >> entry.payloadSize >> entry.payloadOffset;
    entry.width = width == 0 ? 256 : int(width);
    entry.height = height == 0 ? 256 : int(height);
    return entry;
}

// "<w>x<h> planes=<p> bpp=<b> " followed by the payload kind: "png", or
// "dib <header size> <w>x<doubled h>" for a BITMAPINFOHEADER frame.
QString describeFrame(const QByteArray &file, const IconEntry &entry)
{
    const QString header = QStringLiteral("%1x%2 planes=%3 bpp=%4 ")
                               .arg(entry.width)
                               .arg(entry.height)
                               .arg(entry.planes)
                               .arg(entry.bitsPerPixel);
    if (quint64(entry.payloadOffset) + entry.payloadSize > quint64(file.size()))
        return header + QStringLiteral("out of bounds");
    const QByteArray payload = file.mid(qsizetype(entry.payloadOffset), qsizetype(entry.payloadSize));
    if (payload.startsWith(QByteArray("\x89PNG\r\n\x1a\n", 8))) return header + QStringLiteral("png");
    QDataStream dib(payload);
    dib.setByteOrder(QDataStream::LittleEndian);
    quint32 headerSize = 0;
    qint32 width = 0;
    qint32 doubledHeight = 0;
    dib >> headerSize >> width >> doubledHeight;
    return header + QStringLiteral("dib %1 %2x%3").arg(headerSize).arg(width).arg(doubledHeight);
}

QStringList describeFrames(const QByteArray &file, QDataStream &directory, int count)
{
    QStringList frames;
    for (int index = 0; index < count; ++index) frames.append(describeFrame(file, readEntry(directory)));
    return frames;
}
}

class BrandIconGeneratorTest final : public QObject {
    Q_OBJECT

private slots:
    void emitsShellCompatibleFrames();
};

void BrandIconGeneratorTest::emitsShellCompatibleFrames()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString output = directory.filePath(QStringLiteral("generated.ico"));
    QCOMPARE(QProcess::execute(QStringLiteral(SPANTUI_BRAND_ICON_GENERATOR_PATH),
                               {QStringLiteral(SPANTUI_BRAND_SVG_PATH), output}),
             0);

    QFile file(output);
    QVERIFY(file.open(QIODevice::ReadOnly));
    const QByteArray bytes = file.readAll();
    QVERIFY(bytes.size() > 6);

    QDataStream stream(bytes);
    stream.setByteOrder(QDataStream::LittleEndian);
    quint16 reserved = 0;
    quint16 type = 0;
    quint16 count = 0;
    stream >> reserved >> type >> count;
    QCOMPARE(reserved, quint16(0));
    QCOMPARE(type, quint16(1));
    QCOMPARE(count, quint16(7));

    // The shell reads DIB frames up to 128 px and a PNG frame at 256 px.
    QCOMPARE(
        describeFrames(bytes, stream, count),
        (QStringList{
            QStringLiteral("16x16 planes=1 bpp=32 dib 40 16x32"), QStringLiteral("24x24 planes=1 bpp=32 dib 40 24x48"),
            QStringLiteral("32x32 planes=1 bpp=32 dib 40 32x64"), QStringLiteral("48x48 planes=1 bpp=32 dib 40 48x96"),
            QStringLiteral("64x64 planes=1 bpp=32 dib 40 64x128"),
            QStringLiteral("128x128 planes=1 bpp=32 dib 40 128x256"), QStringLiteral("256x256 planes=1 bpp=32 png")}));
}

QTEST_MAIN(BrandIconGeneratorTest)

#include "brand_icon_generator_test.moc"
