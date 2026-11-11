#pragma once

#include <QHostAddress>
#include <QUrl>

class MarkdownNetworkPolicy final {
public:
    static bool isPublicAddress(const QHostAddress &address);
    static bool isUrlAllowed(const QUrl &url);
};
