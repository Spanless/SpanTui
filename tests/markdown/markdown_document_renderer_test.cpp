#include "documents/markdown_document_renderer.h"

#include <QSignalSpy>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTextBlock>
#include <QTextDocument>
#include <QTest>

namespace {
QList<qreal> imageWidths(const QTextDocument &document)
{
    QList<qreal> widths;
    for (QTextBlock block = document.begin(); block.isValid(); block = block.next()) {
        for (auto it = block.begin(); !it.atEnd(); ++it) {
            if (it.fragment().charFormat().isImageFormat())
                widths << it.fragment().charFormat().toImageFormat().width();
        }
    }
    return widths;
}

// Whether each block reading `text` is centered, top to bottom.
QList<bool> centeredBlocksReading(const QTextDocument &document, const QString &text)
{
    QList<bool> centered;
    for (QTextBlock block = document.begin(); block.isValid(); block = block.next()) {
        if (block.text() == text) centered.push_back(block.blockFormat().alignment().testFlag(Qt::AlignHCenter));
    }
    return centered;
}

// Loopback listener that only records which paths were requested; the tests
// assert whether the network was touched, not what the loader decodes.
class RequestRecorder final {
public:
    bool listen()
    {
        QObject::connect(&m_server, &QTcpServer::newConnection, &m_server, [this]() {
            while (QTcpSocket *socket = m_server.nextPendingConnection()) {
                QObject::connect(socket, &QTcpSocket::readyRead, socket, [this, socket]() {
                    const QByteArray line = socket->readLine();
                    const QList<QByteArray> parts = line.split(' ');
                    if (parts.size() > 1) requests.append(QString::fromUtf8(parts.at(1)));
                    socket->write("HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\n"
                                  "Connection: close\r\n\r\n");
                    socket->disconnectFromHost();
                });
                QObject::connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
            }
        });
        return m_server.listen(QHostAddress::LocalHost, 0);
    }

    QString url(const QString &path) const
    {
        return QStringLiteral("http://127.0.0.1:%1%2").arg(m_server.serverPort()).arg(path);
    }

    QStringList requests;

private:
    QTcpServer m_server;
};

MarkdownRemoteImageLoader::NetworkPolicy loopbackPolicy()
{
    MarkdownRemoteImageLoader::NetworkPolicy policy;
    policy.urlAllowed = [](const QUrl &url) {
        return url.scheme() == QStringLiteral("http") && url.host() == QStringLiteral("127.0.0.1");
    };
    policy.addressAllowed = [](const QHostAddress &address) { return address.isLoopback(); };
    return policy;
}
}

class MarkdownDocumentRendererTest final : public QObject {
    Q_OBJECT

private slots:
    void rendersAttachedDocumentWithGfmAndNoHtml();
    void resolvesHeadingAnchors();
    void fitsImagesAndRebuildsForTheme();
    void resolvesHeadingAnchorsInsteadOfSourceLinks();
    void centersOnlyBlocksOwnedByCenteredHtml();
    void reportsNoRemoteImageProgressForPlainMarkdown();
    void skipsHiddenMarkdownAndDiscardsStaleRender();
    void remoteImagesWaitForExplicitAllow();
    void sourceChangeWithdrawsRemoteImageAllow();
    void allowedRemoteImagesStillRefusePrivateHosts();
};

void MarkdownDocumentRendererTest::rendersAttachedDocumentWithGfmAndNoHtml() {
    MarkdownDocumentRenderer renderer;
    QTextDocument document;
    renderer.attachDocument(&document);
    QSignalSpy finished(&renderer, &MarkdownDocumentRenderer::renderingFinished);
    renderer.setSource(QStringLiteral(
        "| Name | Done |\n| --- | --- |\n| item | yes |\n\n"
        "<script>not visible</script>\n\n~~removed~~"));
    QTRY_COMPARE(finished.count(), 1);
    const QString plain = document.toPlainText();
    QVERIFY(plain.contains(QStringLiteral("Name")));
    QVERIFY(plain.contains(QStringLiteral("item")));
    QVERIFY(plain.contains(QStringLiteral("removed")));
    QVERIFY(!plain.contains(QStringLiteral("not visible")));
}

void MarkdownDocumentRendererTest::resolvesHeadingAnchors() {
    MarkdownDocumentRenderer renderer;
    QTextDocument document;
    renderer.attachDocument(&document);
    QSignalSpy finished(&renderer, &MarkdownDocumentRenderer::renderingFinished);
    renderer.setSource(QStringLiteral("# Same\n\n# Same\n"));
    QTRY_COMPARE(finished.count(), 1);
    const int first = renderer.anchorPosition(QStringLiteral("#same"));
    const int second = renderer.anchorPosition(QStringLiteral("#same-1"));
    QVERIFY(first >= 0);
    QVERIFY(second > first);

    // GitHub slugs drop punctuation, keep '_' and '-', and join words with '-'.
    renderer.setSource(QStringLiteral("# Hello, World_x  2!\n"));
    QTRY_COMPARE(finished.count(), 2);
    QVERIFY(renderer.anchorPosition(QStringLiteral("#hello-world_x-2")) >= 0);
}

void MarkdownDocumentRendererTest::fitsImagesAndRebuildsForTheme() {
    MarkdownDocumentRenderer renderer;
    QTextDocument document;
    renderer.attachDocument(&document);
    QSignalSpy finished(&renderer, &MarkdownDocumentRenderer::renderingFinished);
    renderer.setMaximumImageWidth(120);
    renderer.setSource(QStringLiteral("![plain](local.png)\n"));
    QTRY_COMPARE(finished.count(), 1);

    // An image is held to the maximum width.
    QTRY_COMPARE(imageWidths(document), (QList<qreal>{120.0}));

    // A theme switch renders the document again.
    renderer.setDark(!renderer.dark());
    QTRY_COMPARE(finished.count(), 2);
}

void MarkdownDocumentRendererTest::resolvesHeadingAnchorsInsteadOfSourceLinks() {
    QTextDocument document;
    document.setMarkdown(QStringLiteral(
        "[Second repeat](#repeat-1)\n\n"
        "# Repeat\n\n"
        "# Repeat\n"));
    MarkdownDocumentRenderer renderer;
    renderer.attachDocument(&document);

    const int target = renderer.anchorPosition(QStringLiteral("#repeat-1"));
    QVERIFY(target > 0);
    const QTextBlock targetBlock = document.findBlock(target);
    QVERIFY(targetBlock.isValid());
    QCOMPARE(targetBlock.text(), QStringLiteral("Repeat"));
    QCOMPARE(targetBlock.blockNumber(), 2);
    QCOMPARE(renderer.anchorPosition(QStringLiteral("#missing")), -1);
}

void MarkdownDocumentRendererTest::centersOnlyBlocksOwnedByCenteredHtml() {
    MarkdownDocumentRenderer renderer;
    QTextDocument document;
    renderer.attachDocument(&document);
    QSignalSpy finished(&renderer, &MarkdownDocumentRenderer::renderingFinished);
    renderer.setSource(QStringLiteral(
        "<div align=\"center\">Same text</div>\n\nSame text"));
    QTRY_COMPARE(finished.count(), 1);

    QCOMPARE(centeredBlocksReading(document, QStringLiteral("Same text")), (QList<bool>{true, false}));
}

void MarkdownDocumentRendererTest::reportsNoRemoteImageProgressForPlainMarkdown() {
    MarkdownDocumentRenderer renderer;
    QCOMPARE(renderer.pendingRemoteImageCount(), 0);
    QCOMPARE(renderer.remoteImageTotal(), 0);
    QTextDocument document;
    renderer.attachDocument(&document);
    renderer.setSource(QStringLiteral("# hi\n"));
    QCOMPARE(renderer.pendingRemoteImageCount(), 0);
    QCOMPARE(renderer.remoteImageTotal(), 0);
}

void MarkdownDocumentRendererTest::skipsHiddenMarkdownAndDiscardsStaleRender() {
    MarkdownDocumentRenderer renderer;
    QTextDocument document;
    renderer.attachDocument(&document);
    QSignalSpy started(&renderer, &MarkdownDocumentRenderer::renderingStarted);
    QSignalSpy finished(&renderer, &MarkdownDocumentRenderer::renderingFinished);
    renderer.setActive(false);
    renderer.setSource(QStringLiteral("# Hidden\n"));
    QTest::qWait(140);
    QCOMPARE(started.count(), 0);
    QVERIFY(document.isEmpty());

    renderer.setActive(true);
    QTRY_COMPARE_WITH_TIMEOUT(finished.count(), 1, 3000);
    QCOMPARE(document.toPlainText(), QStringLiteral("Hidden"));

    renderer.setSource(QString(400000, QLatin1Char('x')) + QStringLiteral("\n# Stale"));
    const int priorFinished = finished.count();
    renderer.setSource(QStringLiteral("# Current\n"));
    QTRY_VERIFY_WITH_TIMEOUT(finished.count() > priorFinished, 3000);
    QCOMPARE(document.toPlainText(), QStringLiteral("Current"));
}

void MarkdownDocumentRendererTest::remoteImagesWaitForExplicitAllow()
{
    RequestRecorder http;
    QVERIFY(http.listen());
    MarkdownDocumentRenderer renderer;
    renderer.setNetworkPolicyForTesting(loopbackPolicy());
    QTextDocument document;
    renderer.attachDocument(&document);
    QSignalSpy finished(&renderer, &MarkdownDocumentRenderer::renderingFinished);
    renderer.setSource(QStringLiteral("![a](%1)\n\n![b](%2)\n\n![again](%1)\n\n![local](a.png)\n")
                           .arg(http.url(QStringLiteral("/a.png")), http.url(QStringLiteral("/b.png"))));
    QTRY_COMPARE(finished.count(), 1);
    QTRY_COMPARE(renderer.remoteImageCount(), 2);
    QVERIFY(!renderer.remoteImagesAllowed());
    QTest::qWait(100);
    QVERIFY(http.requests.isEmpty());
    QCOMPARE(renderer.remoteImageTotal(), 0);

    renderer.allowRemoteImages();
    QVERIFY(renderer.remoteImagesAllowed());
    QTRY_COMPARE(http.requests.size(), 2);
    http.requests.sort();
    QCOMPARE(http.requests, (QStringList{QStringLiteral("/a.png"), QStringLiteral("/b.png")}));
}

void MarkdownDocumentRendererTest::sourceChangeWithdrawsRemoteImageAllow()
{
    RequestRecorder http;
    QVERIFY(http.listen());
    MarkdownDocumentRenderer renderer;
    renderer.setNetworkPolicyForTesting(loopbackPolicy());
    QTextDocument document;
    renderer.attachDocument(&document);
    QSignalSpy finished(&renderer, &MarkdownDocumentRenderer::renderingFinished);
    renderer.setSource(QStringLiteral("![a](%1)\n").arg(http.url(QStringLiteral("/a.png"))));
    QTRY_COMPARE(finished.count(), 1);
    renderer.allowRemoteImages();
    QTRY_COMPARE(http.requests.size(), 1);

    // Another document (or the same one reopened) asks again.
    renderer.setSource(QStringLiteral("![b](%1)\n").arg(http.url(QStringLiteral("/b.png"))));
    QVERIFY(!renderer.remoteImagesAllowed());
    QTRY_COMPARE(finished.count(), 2);
    QTRY_COMPARE(renderer.remoteImageCount(), 1);
    QTest::qWait(100);
    QCOMPARE(http.requests, QStringList{QStringLiteral("/a.png")});
}

void MarkdownDocumentRendererTest::allowedRemoteImagesStillRefusePrivateHosts()
{
    RequestRecorder http;
    QVERIFY(http.listen());
    MarkdownDocumentRenderer renderer; // default product policy
    QTextDocument document;
    renderer.attachDocument(&document);
    QSignalSpy finished(&renderer, &MarkdownDocumentRenderer::renderingFinished);
    renderer.setSource(QStringLiteral("![a](%1)\n").arg(http.url(QStringLiteral("/a.png"))));
    QTRY_COMPARE(finished.count(), 1);
    QTRY_COMPARE(renderer.remoteImageCount(), 1);
    renderer.allowRemoteImages();
    QTRY_COMPARE(renderer.remoteImageFailedCount(), 1);
    QTest::qWait(100);
    QVERIFY(http.requests.isEmpty());
}

QTEST_MAIN(MarkdownDocumentRendererTest)
#include "markdown_document_renderer_test.moc"
