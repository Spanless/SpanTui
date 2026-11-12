#include "documents/markdown_remote_image_loader.h"

#include <QBuffer>
#include <QHash>
#include <QSignalSpy>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTest>
#include <QTimer>

#include <algorithm>
#include <memory>
namespace {

struct Route {
    int status = 200;
    QByteArray body;
    QList<QPair<QByteArray, QByteArray>> headers;
    int delayMs = 0;
    // Stream until the connection closes instead of declaring a length.
    bool omitLength = false;
};

// A loopback HTTP/1.1 server answering one GET per connection.
class HttpFixture final {
public:
    bool listen()
    {
        QObject::connect(&m_server, &QTcpServer::newConnection, &m_server, [this]() {
            while (QTcpSocket *socket = m_server.nextPendingConnection())
                accept(socket);
        });
        return m_server.listen(QHostAddress::LocalHost, 0);
    }

    QUrl url(const QString &path) const
    {
        return QUrl(QStringLiteral("http://127.0.0.1:%1%2").arg(m_server.serverPort()).arg(path));
    }

    void route(const QString &path, const Route &route) { m_routes.insert(path, route); }

    void redirect(const QString &path, const QByteArray &location)
    {
        Route route;
        route.status = 302;
        route.headers = {{"Location", location}};
        m_routes.insert(path, route);
    }

    QStringList requests;
    int active = 0;
    int maxActive = 0;

private:
    void accept(QTcpSocket *socket)
    {
        QObject::connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
        auto buffer = std::make_shared<QByteArray>();
        QObject::connect(socket, &QTcpSocket::readyRead, socket, [this, socket, buffer]() {
            const bool complete = buffer->contains("\r\n\r\n");
            buffer->append(socket->readAll());
            if (complete || !buffer->contains("\r\n\r\n"))
                return;
            const QList<QByteArray> requestLine = buffer->left(buffer->indexOf("\r\n")).split(' ');
            const QString path = requestLine.size() > 1 ? QString::fromUtf8(requestLine.at(1))
                                                        : QString();
            requests.append(path);
            ++active;
            maxActive = std::max(maxActive, active);
            Route route = m_routes.value(path, Route{404});
            QTimer::singleShot(route.delayMs, socket, [this, socket, route]() {
                --active;
                socket->write(response(route));
                socket->disconnectFromHost();
            });
        });
    }

    static QByteArray response(const Route &route)
    {
        static const QHash<int, QByteArray> reasons{
            {200, "OK"}, {302, "Found"}, {404, "Not Found"}};
        QByteArray bytes = "HTTP/1.1 " + QByteArray::number(route.status) + ' '
            + reasons.value(route.status, "Status") + "\r\n";
        bool declaredLength = false;
        for (const auto &[name, value] : route.headers) {
            bytes += name + ": " + value + "\r\n";
            declaredLength |= name.compare("Content-Length", Qt::CaseInsensitive) == 0;
        }
        if (!declaredLength && !route.omitLength)
            bytes += "Content-Length: " + QByteArray::number(route.body.size()) + "\r\n";
        bytes += "Connection: close\r\n\r\n";
        return bytes + route.body;
    }

    QTcpServer m_server;
    QHash<QString, Route> m_routes;
};

QByteArray png(const QSize &size, const QColor &color, QImage::Format format = QImage::Format_ARGB32)
{
    QImage image(size, format);
    if (format == QImage::Format_Mono) {
        image.setColorTable({color.rgb(), qRgb(255, 255, 255)});
        image.fill(0);
    } else {
        image.fill(color);
    }
    QByteArray bytes;
    QBuffer buffer(&bytes);
    buffer.open(QIODevice::WriteOnly);
    image.save(&buffer, "PNG");
    return bytes;
}

Route image(const QColor &color = Qt::red, int delayMs = 0)
{
    Route route;
    route.body = png(QSize(3, 2), color);
    route.headers = {{"Content-Type", "image/png"}};
    route.delayMs = delayMs;
    return route;
}

// Routes `count` images at "/<stem><n>.png", the n-th answered with
// routeFor(n); returns their URLs.
template <typename RouteFor>
QSet<QUrl> routeImages(HttpFixture &http, const QString &stem, int count, RouteFor routeFor)
{
    QSet<QUrl> urls;
    for (int index = 0; index < count; ++index) {
        const QString path = QStringLiteral("/%1%2.png").arg(stem).arg(index);
        http.route(path, routeFor(index));
        urls.insert(http.url(path));
    }
    return urls;
}

// The product policy with loopback admitted, so the fixture is reachable.
// Paths containing "blocked" stand in for a URL the policy refuses.
MarkdownRemoteImageLoader::NetworkPolicy loopbackPolicy()
{
    MarkdownRemoteImageLoader::NetworkPolicy policy;
    policy.urlAllowed = [](const QUrl &url) {
        return url.scheme() == QStringLiteral("http")
            && url.host() == QStringLiteral("127.0.0.1")
            && !url.path().contains(QStringLiteral("blocked"));
    };
    policy.addressAllowed = [](const QHostAddress &address) { return address.isLoopback(); };
    return policy;
}

} // namespace

class MarkdownRemoteImageLoaderTest final : public QObject {
    Q_OBJECT

private slots:
    void init();
    void cleanup();
    void productPolicyRejectsLoopbackWithoutNetwork();
    void downloadsDecodesAndCaches();
    void followsAtMostThreeRedirects();
    void revalidatesEveryRedirectTarget();
    void enforcesDownloadSizeLimit();
    void rejectsUnusableResponses();
    void limitsConcurrentRequests();
    void cancelStopsDelivery();
    void failedImagesWaitForRetry();
    void evictsLeastRecentlyUsedAndRedownloads();

private:
    std::unique_ptr<HttpFixture> m_http;
    std::unique_ptr<MarkdownRemoteImageLoader> m_loader;
};

void MarkdownRemoteImageLoaderTest::init()
{
    m_http = std::make_unique<HttpFixture>();
    QVERIFY(m_http->listen());
    m_loader = std::make_unique<MarkdownRemoteImageLoader>();
    m_loader->setNetworkPolicyForTesting(loopbackPolicy());
}

void MarkdownRemoteImageLoaderTest::cleanup()
{
    m_loader.reset();
    m_http.reset();
}

void MarkdownRemoteImageLoaderTest::productPolicyRejectsLoopbackWithoutNetwork()
{
    MarkdownRemoteImageLoader loader;  // default product policy
    m_http->route(QStringLiteral("/a.png"), image());
    const QSet<QUrl> urls{m_http->url(QStringLiteral("/a.png")),
                          QUrl(QStringLiteral("file:///C:/a.png"))};
    loader.request(urls);
    QCOMPARE(loader.totalCount(), 2);
    QCOMPARE(loader.failedCount(), 2);
    QCOMPARE(loader.pendingCount(), 0);
    QTest::qWait(50);
    QVERIFY(m_http->requests.isEmpty());

    // Policy failures are permanent: asking again never reaches the network.
    loader.request(urls);
    QTest::qWait(50);
    QCOMPARE(loader.failedCount(), 2);
    QVERIFY(m_http->requests.isEmpty());
}

void MarkdownRemoteImageLoaderTest::downloadsDecodesAndCaches()
{
    m_http->route(QStringLiteral("/red.png"), image(Qt::red));
    const QUrl url = m_http->url(QStringLiteral("/red.png"));
    QSignalSpy ready(m_loader.get(), &MarkdownRemoteImageLoader::imageReady);

    m_loader->request({url});
    QCOMPARE(m_loader->pendingCount(), 1);
    QTRY_COMPARE_WITH_TIMEOUT(ready.count(), 1, 10000);
    QCOMPARE(ready.at(0).at(0).toUrl(), url);
    const QImage decoded = ready.at(0).at(1).value<QImage>();
    QCOMPARE(decoded.size(), QSize(3, 2));
    QCOMPARE(m_loader->pendingCount(), 0);
    QCOMPARE(m_loader->failedCount(), 0);

    // The same document asking again gets nothing new.
    m_loader->request({url});
    QTest::qWait(50);
    QCOMPARE(ready.count(), 1);

    // A document that shows the image again is served from the cache.
    m_loader->request({});
    QCOMPARE(m_loader->totalCount(), 0);
    m_loader->request({url});
    QCOMPARE(ready.count(), 2);
    QCOMPARE(m_loader->pendingCount(), 0);
    QCOMPARE(m_http->requests, QStringList{QStringLiteral("/red.png")});
}

void MarkdownRemoteImageLoaderTest::followsAtMostThreeRedirects()
{
    m_http->redirect(QStringLiteral("/hop1"), "/hop2");
    m_http->redirect(QStringLiteral("/hop2"), "/hop3");
    m_http->redirect(QStringLiteral("/hop3"), "/three.png");
    m_http->route(QStringLiteral("/three.png"), image());
    m_http->redirect(QStringLiteral("/long1"), "/long2");
    m_http->redirect(QStringLiteral("/long2"), "/long3");
    m_http->redirect(QStringLiteral("/long3"), "/long4");
    m_http->redirect(QStringLiteral("/long4"), "/four.png");
    m_http->route(QStringLiteral("/four.png"), image());
    QSignalSpy ready(m_loader.get(), &MarkdownRemoteImageLoader::imageReady);

    const QUrl three = m_http->url(QStringLiteral("/hop1"));
    m_loader->request({three, m_http->url(QStringLiteral("/long1"))});
    QTRY_VERIFY_WITH_TIMEOUT(m_loader->pendingCount() == 0, 10000);
    // The image is delivered under the URL the document asked for.
    QCOMPARE(ready.count(), 1);
    QCOMPARE(ready.at(0).at(0).toUrl(), three);
    QCOMPARE(m_loader->failedCount(), 1);
    QVERIFY(m_http->requests.contains(QStringLiteral("/long4")));
    QVERIFY(!m_http->requests.contains(QStringLiteral("/four.png")));
}

void MarkdownRemoteImageLoaderTest::revalidatesEveryRedirectTarget()
{
    m_http->redirect(QStringLiteral("/to-policy"), "/blocked.png");
    m_http->route(QStringLiteral("/blocked.png"), image());
    m_http->redirect(QStringLiteral("/to-file"), "file:///C:/secret.png");
    QSignalSpy ready(m_loader.get(), &MarkdownRemoteImageLoader::imageReady);

    m_loader->request({m_http->url(QStringLiteral("/to-policy")),
                       m_http->url(QStringLiteral("/to-file"))});
    QTRY_COMPARE_WITH_TIMEOUT(m_loader->failedCount(), 2, 10000);
    QCOMPARE(ready.count(), 0);
    QVERIFY(!m_http->requests.contains(QStringLiteral("/blocked.png")));
}

void MarkdownRemoteImageLoaderTest::enforcesDownloadSizeLimit()
{
    constexpr int kLimit = 10 * 1024 * 1024;
    Route declared = image();
    declared.headers.append({"Content-Length", QByteArray::number(kLimit + 1)});
    m_http->route(QStringLiteral("/declared-huge.png"), declared);
    // An image that would decode fine, padded past the limit without a
    // declared length: the stream itself is cut off.
    Route streamed = image();
    streamed.body += QByteArray(kLimit, '\0');
    streamed.omitLength = true;
    m_http->route(QStringLiteral("/streamed-huge.png"), streamed);
    QSignalSpy ready(m_loader.get(), &MarkdownRemoteImageLoader::imageReady);

    m_loader->request({m_http->url(QStringLiteral("/declared-huge.png")),
                       m_http->url(QStringLiteral("/streamed-huge.png"))});
    QTRY_COMPARE_WITH_TIMEOUT(m_loader->failedCount(), 2, 15000);
    QCOMPARE(ready.count(), 0);
}

void MarkdownRemoteImageLoaderTest::rejectsUnusableResponses()
{
    Route garbage;
    garbage.body = "definitely not an image";
    m_http->route(QStringLiteral("/garbage.png"), garbage);
    m_http->route(QStringLiteral("/empty.png"), Route{});
    Route notFound = image();
    notFound.status = 404;
    m_http->route(QStringLiteral("/not-found.png"), notFound);
    // Decode limits: one side over 8192 pixels, or over 16 Mi pixels total.
    // Indexed images keep the fixtures small while declaring large sizes.
    Route wide;
    wide.body = png(QSize(8193, 1), Qt::black, QImage::Format_Mono);
    m_http->route(QStringLiteral("/too-wide.png"), wide);
    Route dense;
    dense.body = png(QSize(4100, 4100), Qt::black, QImage::Format_Mono);
    m_http->route(QStringLiteral("/too-many-pixels.png"), dense);
    QSignalSpy ready(m_loader.get(), &MarkdownRemoteImageLoader::imageReady);

    m_loader->request({m_http->url(QStringLiteral("/garbage.png")),
                       m_http->url(QStringLiteral("/empty.png")),
                       m_http->url(QStringLiteral("/not-found.png")),
                       m_http->url(QStringLiteral("/too-wide.png")),
                       m_http->url(QStringLiteral("/too-many-pixels.png"))});
    QTRY_COMPARE_WITH_TIMEOUT(m_loader->failedCount(), 5, 15000);
    QCOMPARE(m_loader->pendingCount(), 0);
    QCOMPARE(ready.count(), 0);
}

void MarkdownRemoteImageLoaderTest::limitsConcurrentRequests()
{
    const QSet<QUrl> urls = routeImages(*m_http, QStringLiteral("slow-"), 6, [](int) { return image(Qt::blue, 100); });
    QSignalSpy ready(m_loader.get(), &MarkdownRemoteImageLoader::imageReady);
    m_loader->request(urls);
    QTRY_COMPARE_WITH_TIMEOUT(ready.count(), 6, 15000);
    QCOMPARE(m_http->maxActive, 3);
}

void MarkdownRemoteImageLoaderTest::cancelStopsDelivery()
{
    m_http->route(QStringLiteral("/slow.png"), image(Qt::green, 150));
    QSignalSpy ready(m_loader.get(), &MarkdownRemoteImageLoader::imageReady);
    QSignalSpy cancelled(m_loader.get(), &MarkdownRemoteImageLoader::cancelled);

    m_loader->request({m_http->url(QStringLiteral("/slow.png"))});
    QTRY_COMPARE_WITH_TIMEOUT(m_http->requests.size(), 1, 10000);
    m_loader->cancel();
    QCOMPARE(cancelled.count(), 1);
    QCOMPARE(m_loader->pendingCount(), 0);
    QCOMPARE(m_loader->totalCount(), 0);
    // Past the response delay: a delivery would have arrived by now.
    QTest::qWait(300);
    QCOMPARE(ready.count(), 0);

    // Nothing to cancel: no second notification.
    m_loader->cancel();
    QCOMPARE(cancelled.count(), 1);
}

void MarkdownRemoteImageLoaderTest::failedImagesWaitForRetry()
{
    Route missing;
    missing.status = 404;
    m_http->route(QStringLiteral("/missing.png"), missing);
    const QUrl url = m_http->url(QStringLiteral("/missing.png"));

    m_loader->request({url});
    QTRY_COMPARE_WITH_TIMEOUT(m_loader->failedCount(), 1, 10000);
    // A re-render of the same document does not hammer a failing server.
    m_loader->request({url});
    QTest::qWait(50);
    QCOMPARE(m_http->requests.size(), 1);
    QCOMPARE(m_loader->failedCount(), 1);

    // A document that stopped showing the image forgets the failure; showing
    // it again is a fresh attempt.
    m_loader->request({});
    QCOMPARE(m_loader->failedCount(), 0);
    m_loader->request({url});
    QTRY_COMPARE_WITH_TIMEOUT(m_http->requests.size(), 2, 10000);
}

void MarkdownRemoteImageLoaderTest::evictsLeastRecentlyUsedAndRedownloads()
{
    // Two cached images instead of the product's 32: the third evicts one.
    m_loader->setMaximumCachedImagesForTesting(2);
    const QSet<QUrl> urls =
        routeImages(*m_http, QStringLiteral("n"), 3, [](int index) { return image(QColor::fromRgb(index, 0, 0)); });
    QSignalSpy ready(m_loader.get(), &MarkdownRemoteImageLoader::imageReady);
    QSignalSpy evicted(m_loader.get(), &MarkdownRemoteImageLoader::imageEvicted);
    m_loader->request(urls);
    QTRY_COMPARE_WITH_TIMEOUT(ready.count(), 3, 10000);
    QCOMPARE(evicted.count(), 1);
    QCOMPARE(m_loader->failedCount(), 0);

    // Eviction is a cache decision, not a failure: the image downloads again.
    const QUrl dropped = evicted.at(0).at(0).toUrl();
    QVERIFY(urls.contains(dropped));
    m_loader->request({dropped});
    QTRY_COMPARE_WITH_TIMEOUT(ready.count(), 4, 10000);
    QCOMPARE(ready.constLast().at(0).toUrl(), dropped);
    QCOMPARE(m_http->requests.count(dropped.path()), 2);
}

QTEST_MAIN(MarkdownRemoteImageLoaderTest)
#include "markdown_remote_image_loader_test.moc"
