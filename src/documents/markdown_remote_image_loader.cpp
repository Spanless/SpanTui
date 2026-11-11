#include "documents/markdown_remote_image_loader.h"

#include <QBuffer>
#include <QDateTime>
#include <QHostInfo>
#include <QImageReader>
#include <QNetworkReply>
#include <QSharedPointer>

#include <algorithm>
#include <limits>

namespace {
constexpr qint64 kMaximumRemoteImageBytes = 10 * 1024 * 1024;
constexpr qint64 kMaximumDecodedImageBytes = 64 * 1024 * 1024;
constexpr qint64 kMaximumImageCacheBytes = 64 * 1024 * 1024;
constexpr qsizetype kMaximumCachedImages = 32;
constexpr int kMaximumImageDimension = 8192;
constexpr int kMaximumImagePixels = 16 * 1024 * 1024;
constexpr int kMaximumConcurrentImageRequests = 3;
constexpr int kMaximumImageRedirects = 3;
// Transient network/decode failures may be retried after this cool-down; a
// permanent blank is only correct for policy-rejected URLs.
constexpr qint64 kFailureRetryCooldownMs = 30'000;

bool decodeImage(const QByteArray &bytes, QImage *image) {
    QBuffer buffer;
    buffer.setData(bytes);
    if (!buffer.open(QIODevice::ReadOnly))
        return false;
    QImageReader reader(&buffer);
    reader.setDecideFormatFromContent(true);
    const QSize size = reader.size();
    if (!size.isValid() || size.width() <= 0 || size.height() <= 0
        || size.width() > kMaximumImageDimension
        || size.height() > kMaximumImageDimension
        || qint64(size.width()) * size.height() > kMaximumImagePixels) {
        return false;
    }
    QImage decoded = reader.read();
    if (decoded.isNull()
        || qint64(decoded.sizeInBytes()) > kMaximumDecodedImageBytes) {
        return false;
    }
    *image = std::move(decoded);
    return true;
}
}

MarkdownRemoteImageLoader::MarkdownRemoteImageLoader(QObject *parent)
    : QObject(parent), m_network(this), m_maximumCachedImages(kMaximumCachedImages) {}

void MarkdownRemoteImageLoader::setNetworkPolicyForTesting(NetworkPolicy policy) {
    m_policy = std::move(policy);
}

void MarkdownRemoteImageLoader::setMaximumCachedImagesForTesting(qsizetype count) {
    m_maximumCachedImages = count;
}

int MarkdownRemoteImageLoader::pendingCount() const { return m_pending.size(); }
int MarkdownRemoteImageLoader::totalCount() const { return m_current.size(); }
int MarkdownRemoteImageLoader::failedCount() const { return m_failed.size(); }

void MarkdownRemoteImageLoader::request(const QSet<QUrl> &urls) {
    const QSet<QUrl> previous = m_current;
    m_current = urls;
    m_failed.intersect(urls);
    for (auto iterator = m_failedAt.begin(); iterator != m_failedAt.end();) {
        if (!urls.contains(iterator.key()))
            iterator = m_failedAt.erase(iterator);
        else
            ++iterator;
    }
    for (auto iterator = m_pending.begin(); iterator != m_pending.end();) {
        if (!urls.contains(*iterator))
            iterator = m_pending.erase(iterator);
        else
            ++iterator;
    }
    for (const QUrl &url : urls) {
        if (m_pending.contains(url))
            continue;
        if (m_failed.contains(url)) {
            if (failedRetryBlocked(url))
                continue;
            m_failed.remove(url);
            m_failedAt.remove(url);
        }
        if (m_cache.contains(url)) {
            touch(url);
            if (!previous.contains(url))
                emit imageReady(url, m_cache.value(url));
            continue;
        }
        if (!m_policy.urlAllowed(url)) {
            markFailed(url, true);
            continue;
        }
        m_pending.insert(url);
        m_queue.enqueue(url);
    }
    emit progressChanged();
    pump();
}

void MarkdownRemoteImageLoader::cancel() {
    const bool hadRequests = !m_current.isEmpty() || !m_pending.isEmpty();
    ++m_generation;
    m_current.clear();
    m_queue.clear();
    m_pending.clear();
    m_failed.clear();
    m_failedAt.clear();
    const auto replies = m_network.findChildren<QNetworkReply *>();
    for (QNetworkReply *reply : replies)
        reply->abort();
    if (hadRequests)
        emit cancelled();
    emit progressChanged();
}

void MarkdownRemoteImageLoader::pump() {
    while (m_activeRequests < kMaximumConcurrentImageRequests && !m_queue.isEmpty()) {
        const QUrl url = m_queue.dequeue();
        if (!m_current.contains(url) || !m_pending.contains(url))
            continue;
        ++m_activeRequests;
        resolve(url, url, 0, m_generation);
    }
}

void MarkdownRemoteImageLoader::resolve(const QUrl &originalUrl,
                                        const QUrl &requestUrl,
                                        int redirectCount,
                                        quint64 generation) {
    if (generation != m_generation
        || !m_policy.urlAllowed(requestUrl)) {
        finish(originalUrl, {}, generation);
        return;
    }
    QHostAddress literal;
    if (literal.setAddress(requestUrl.host())) {
        if (!m_policy.addressAllowed(literal)) {
            finish(originalUrl, {}, generation);
            return;
        }
        startRequest(originalUrl, requestUrl, redirectCount, literal, generation);
        return;
    }
    QHostInfo::lookupHost(
        requestUrl.host(), this,
        [this, originalUrl, requestUrl, redirectCount,
         generation](const QHostInfo &hostInfo) {
            if (generation != m_generation) {
                finish(originalUrl, {}, generation);
                return;
            }
            if (hostInfo.error() != QHostInfo::NoError || hostInfo.addresses().isEmpty()) {
                finish(originalUrl, {}, generation);
                return;
            }
            QHostAddress pinned;
            for (const QHostAddress &address : hostInfo.addresses()) {
                if (!m_policy.addressAllowed(address)) {
                    finish(originalUrl, {}, generation);
                    return;
                }
                if (pinned.isNull())
                    pinned = address;
            }
            if (pinned.isNull()) {
                finish(originalUrl, {}, generation);
                return;
            }
            // Connect to the resolved IP so a second DNS lookup cannot rebind
            // to a private address after the public check.
            startRequest(originalUrl, requestUrl, redirectCount, pinned, generation);
        });
}

void MarkdownRemoteImageLoader::startRequest(const QUrl &originalUrl,
                                             const QUrl &requestUrl,
                                             int redirectCount,
                                             const QHostAddress &pinnedAddress,
                                             quint64 generation) {
    if (generation != m_generation) {
        finish(originalUrl, {}, generation);
        return;
    }
    QUrl connectUrl = requestUrl;
    connectUrl.setHost(pinnedAddress.toString());
    QNetworkRequest request(connectUrl);
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                         QNetworkRequest::ManualRedirectPolicy);
    request.setTransferTimeout(15000);
    request.setRawHeader("User-Agent", "SpanTui-MarkdownPreview/1");
    // Keep the original host for virtual hosting / SNI while connecting by IP.
    request.setRawHeader("Host", requestUrl.host().toUtf8());
    if (requestUrl.scheme() == QStringLiteral("https"))
        request.setPeerVerifyName(requestUrl.host());
    QNetworkReply *reply = m_network.get(request);
    auto bytes = QSharedPointer<QByteArray>::create();
    auto rejected = QSharedPointer<bool>::create(false);
    const auto consumeAvailable = [reply, bytes, rejected]() {
        if (*rejected)
            return;
        const QByteArray chunk = reply->readAll();
        if (bytes->size() + chunk.size() > kMaximumRemoteImageBytes) {
            *rejected = true;
            bytes->clear();
            reply->abort();
            return;
        }
        bytes->append(chunk);
    };
    connect(reply, &QNetworkReply::metaDataChanged, this,
            [reply, rejected]() {
                const QVariant contentLength =
                    reply->header(QNetworkRequest::ContentLengthHeader);
                if (contentLength.isValid()
                    && contentLength.toLongLong() > kMaximumRemoteImageBytes) {
                    *rejected = true;
                    reply->abort();
                }
            });
    connect(reply, &QIODevice::readyRead, this, consumeAvailable);
    connect(reply, &QNetworkReply::finished, this,
            [this, reply, originalUrl, requestUrl, redirectCount, bytes,
             rejected, consumeAvailable, generation]() {
                consumeAvailable();
                const QVariant redirect =
                    reply->attribute(QNetworkRequest::RedirectionTargetAttribute);
                const QNetworkReply::NetworkError error = reply->error();
                reply->deleteLater();
                if (!*rejected && redirect.isValid()
                    && redirectCount < kMaximumImageRedirects) {
                    const QUrl redirected = requestUrl.resolved(redirect.toUrl());
                    if (requestUrl.scheme() == QStringLiteral("https")
                        && redirected.scheme() != QStringLiteral("https")) {
                        finish(originalUrl, {}, generation);
                        return;
                    }
                    if (!m_policy.urlAllowed(redirected)) {
                        finish(originalUrl, {}, generation);
                        return;
                    }
                    resolve(originalUrl, redirected, redirectCount + 1, generation);
                    return;
                }
                QImage image;
                if (!*rejected && error == QNetworkReply::NoError
                    && !redirect.isValid() && !bytes->isEmpty()) {
                    decodeImage(*bytes, &image);
                }
                finish(originalUrl, image, generation);
            });
}

void MarkdownRemoteImageLoader::finish(const QUrl &url, const QImage &image,
                                       quint64 generation) {
    m_activeRequests = std::max(0, m_activeRequests - 1);
    if (generation != m_generation) {
        pump();
        return;
    }
    m_pending.remove(url);
    if (image.isNull()) {
        markFailed(url, false);
    } else {
        cache(url, image);
        if (m_current.contains(url))
            emit imageReady(url, image);
    }
    emit progressChanged();
    pump();
}

void MarkdownRemoteImageLoader::markFailed(const QUrl &url, bool permanent)
{
    m_failed.insert(url);
    m_failedAt.insert(url, permanent ? std::numeric_limits<qint64>::max()
                                     : QDateTime::currentMSecsSinceEpoch());
}

bool MarkdownRemoteImageLoader::failedRetryBlocked(const QUrl &url) const
{
    const qint64 failedAt = m_failedAt.value(url, 0);
    if (failedAt == std::numeric_limits<qint64>::max()) {
        return true;
    }
    return QDateTime::currentMSecsSinceEpoch() - failedAt
        < kFailureRetryCooldownMs;
}

void MarkdownRemoteImageLoader::cache(const QUrl &url, const QImage &image) {
    if (image.isNull() || qint64(image.sizeInBytes()) > kMaximumImageCacheBytes)
        return;
    if (m_cache.contains(url))
        m_cacheBytes -= qint64(m_cache.value(url).sizeInBytes());
    m_cache.insert(url, image);
    m_cacheBytes += qint64(image.sizeInBytes());
    touch(url);
    while ((m_cacheBytes > kMaximumImageCacheBytes
            || m_cacheOrder.size() > m_maximumCachedImages)
           && !m_cacheOrder.isEmpty()) {
        const QUrl evicted = m_cacheOrder.takeFirst();
        const auto iterator = m_cache.find(evicted);
        if (iterator == m_cache.end())
            continue;
        m_cacheBytes -= qint64(iterator.value().sizeInBytes());
        m_cache.erase(iterator);
        // Eviction is a cache decision, not a failure: the next request must
        // be able to download the image again instead of blanking it forever.
        emit imageEvicted(evicted);
    }
}

void MarkdownRemoteImageLoader::touch(const QUrl &url) {
    m_cacheOrder.removeAll(url);
    m_cacheOrder.push_back(url);
}
