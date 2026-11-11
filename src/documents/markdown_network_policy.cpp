#include "documents/markdown_network_policy.h"

#include <QAbstractSocket>
#include <QList>
#include <QPair>

#include <algorithm>

bool MarkdownNetworkPolicy::isPublicAddress(const QHostAddress &address) {
    // IPv4-mapped IPv6 (::ffff:10.0.0.1) must become IPv4 first: Qt's
    // isGlobal() calls it global and the IPv4 subnet table never matches it.
    QHostAddress normalized = address;
    if (address.protocol() == QAbstractSocket::IPv6Protocol) {
        bool mapped = false;
        const quint32 ipv4 = address.toIPv4Address(&mapped);
        if (mapped) {
            normalized = QHostAddress(ipv4);
        }
    }
    if (!normalized.isGlobal())
        return false;
    if (normalized.protocol() == QAbstractSocket::IPv4Protocol) {
        static const QList<QPair<QHostAddress, int>> blockedSubnets{
            {QHostAddress(QStringLiteral("0.0.0.0")), 8},
            {QHostAddress(QStringLiteral("10.0.0.0")), 8},
            {QHostAddress(QStringLiteral("100.64.0.0")), 10},
            {QHostAddress(QStringLiteral("172.16.0.0")), 12},
            {QHostAddress(QStringLiteral("192.168.0.0")), 16},
            {QHostAddress(QStringLiteral("198.18.0.0")), 15}
        };
        return std::none_of(blockedSubnets.cbegin(), blockedSubnets.cend(),
                            [&normalized](const auto &subnet) {
                                return normalized.isInSubnet(subnet.first, subnet.second);
                            });
    }
    // IPv6 unique-local addresses are not covered by the IPv4 table.
    static const QList<QPair<QHostAddress, int>> blockedV6Subnets{
        {QHostAddress(QStringLiteral("fc00::")), 7},
        {QHostAddress(QStringLiteral("fe80::")), 10}
    };
    return std::none_of(blockedV6Subnets.cbegin(), blockedV6Subnets.cend(),
                        [&normalized](const auto &subnet) {
                            return normalized.isInSubnet(subnet.first, subnet.second);
                        });
}

bool MarkdownNetworkPolicy::isUrlAllowed(const QUrl &url) {
    if (!url.isValid()
        || (url.scheme() != QStringLiteral("https")
            && url.scheme() != QStringLiteral("http"))
        || url.host().isEmpty() || !url.userInfo().isEmpty()) {
        return false;
    }
    QString host = url.host().toLower();
    while (host.endsWith(QLatin1Char('.')))
        host.chop(1);
    if (host == QStringLiteral("localhost")
        || host.endsWith(QStringLiteral(".localhost"))
        || host.endsWith(QStringLiteral(".local"))
        || host.endsWith(QStringLiteral(".internal"))) {
        return false;
    }
    QHostAddress literal;
    return !literal.setAddress(host) || isPublicAddress(literal);
}
