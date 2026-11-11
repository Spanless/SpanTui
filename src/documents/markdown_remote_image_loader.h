#pragma once

#include "documents/markdown_network_policy.h"

#include <QHash>
#include <QHostAddress>
#include <QImage>
#include <QList>
#include <QNetworkAccessManager>
#include <QQueue>
#include <QSet>
#include <QUrl>

#include <functional>

class MarkdownRemoteImageLoader final : public QObject {
    Q_OBJECT

public:
    // The URL and resolved-address checks applied on every hop.
    struct NetworkPolicy {
        std::function<bool(const QUrl &)> urlAllowed = MarkdownNetworkPolicy::isUrlAllowed;
        std::function<bool(const QHostAddress &)> addressAllowed =
            MarkdownNetworkPolicy::isPublicAddress;
    };

    explicit MarkdownRemoteImageLoader(QObject *parent = nullptr);

    // Tests serve images from a loopback fixture, which the product policy
    // refuses by design.
    void setNetworkPolicyForTesting(NetworkPolicy policy);
    // Lowers the cached-image count so eviction needs only a few downloads.
    void setMaximumCachedImagesForTesting(qsizetype count);

    int pendingCount() const;
    int totalCount() const;
    int failedCount() const;
    void request(const QSet<QUrl> &urls);
    void cancel();

signals:
    void imageReady(const QUrl &url, const QImage &image);
    void imageEvicted(const QUrl &url);
    void progressChanged();
    void cancelled();

private:
    void pump();
    void resolve(const QUrl &originalUrl, const QUrl &requestUrl,
                 int redirectCount, quint64 generation);
    void startRequest(const QUrl &originalUrl, const QUrl &requestUrl,
                      int redirectCount, const QHostAddress &pinnedAddress,
                      quint64 generation);
    void finish(const QUrl &url, const QImage &image, quint64 generation);
    void cache(const QUrl &url, const QImage &image);
    void touch(const QUrl &url);
    bool failedRetryBlocked(const QUrl &url) const;
    void markFailed(const QUrl &url, bool permanent);

    NetworkPolicy m_policy;
    QNetworkAccessManager m_network;
    QSet<QUrl> m_pending;
    QSet<QUrl> m_failed;
    // Wall-clock timestamp of the last failure. Policy-rejected URLs store the
    // maximum value and are never retried; transient network/decode failures
    // may be retried after a cool-down instead of blanking the image forever.
    QHash<QUrl, qint64> m_failedAt;
    QSet<QUrl> m_current;
    QQueue<QUrl> m_queue;
    QHash<QUrl, QImage> m_cache;
    QList<QUrl> m_cacheOrder;
    qint64 m_cacheBytes = 0;
    qsizetype m_maximumCachedImages;
    int m_activeRequests = 0;
    quint64 m_generation = 0;
};
