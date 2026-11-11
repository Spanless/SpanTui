#include "documents/markdown_network_policy.h"

#include <QTest>

class MarkdownNetworkPolicyTest final : public QObject {
    Q_OBJECT

private slots:
    void rejectsUnsafeUrls_data();
    void rejectsUnsafeUrls();
    void acceptsPublicUrls_data();
    void acceptsPublicUrls();
    void classifiesResolvedAddresses_data();
    void classifiesResolvedAddresses();
};

void MarkdownNetworkPolicyTest::rejectsUnsafeUrls_data() {
    QTest::addColumn<QUrl>("url");
    const auto row = [](const char *name, const char *url) {
        QTest::newRow(name) << QUrl(QString::fromLatin1(url));
    };
    row("loopback", "http://127.0.0.1/a.png");
    row("loopback-v6", "http://[::1]/a.png");
    row("unspecified", "http://0.0.0.0/a.png");
    row("private-v4", "https://10.0.0.1/a.png");
    row("cloud-metadata", "http://169.254.169.254/latest/meta-data");
    row("mapped-private-v4", "http://[::ffff:10.0.0.1]/a.png");
    row("mapped-private-192", "http://[::ffff:192.168.1.1]/a.png");
    row("mapped-carrier-grade", "http://[::ffff:100.64.0.1]/a.png");
    row("unique-local-v6", "http://[fd00::1]/a.png");
    row("unique-local-fc00", "http://[fc00::1]/a.png");
    row("link-local-v6", "http://[fe80::1]/a.png");
    row("localhost", "https://localhost/a.png");
    row("localhost-uppercase", "https://LOCALHOST/a.png");
    row("localhost-trailing-dot", "https://localhost./a.png");
    row("localhost-subdomain", "https://api.localhost/a.png");
    row("local-suffix", "https://host.local/a.png");
    row("local-suffix-trailing-dot", "https://printer.local./a.png");
    row("internal-suffix", "https://svc.internal/a.png");
    row("credentials", "https://user@example.test/a.png");
    row("credentials-with-password", "https://user:secret@example.test/a.png");
    row("file", "file:///tmp/a.png");
    row("downgraded-scheme", "ftp://example.test/a.png");
    row("data-uri", "data:image/png;base64,AAAA");
    row("no-host", "https:///a.png");
    row("relative", "images/a.png");
}

void MarkdownNetworkPolicyTest::rejectsUnsafeUrls() {
    QFETCH(QUrl, url);
    QVERIFY(!MarkdownNetworkPolicy::isUrlAllowed(url));
}

void MarkdownNetworkPolicyTest::acceptsPublicUrls_data() {
    QTest::addColumn<QUrl>("url");
    const auto row = [](const char *name, const char *url) {
        QTest::newRow(name) << QUrl(QString::fromLatin1(url));
    };
    // Host names pass here; their resolved addresses are checked separately.
    row("public-host", "https://example.test/a.png");
    row("public-host-port", "https://example.test:8443/a.png");
    row("local-is-only-a-suffix", "https://notlocal.test/a.png");
    row("public-v4-http", "http://8.8.8.8/a.png");
    row("public-v4-https", "https://8.8.8.8/a.png");
    row("public-v6", "https://[2606:4700::1111]/a.png");
}

void MarkdownNetworkPolicyTest::acceptsPublicUrls() {
    QFETCH(QUrl, url);
    QVERIFY(MarkdownNetworkPolicy::isUrlAllowed(url));
}

void MarkdownNetworkPolicyTest::classifiesResolvedAddresses_data() {
    // The loader pins every DNS answer through isPublicAddress, so this is
    // the boundary that stops a public name resolving to an internal host.
    QTest::addColumn<QString>("address");
    QTest::addColumn<bool>("isPublic");
    const auto row = [](const char *address, bool isPublic) {
        QTest::newRow(address) << QString::fromLatin1(address) << isPublic;
    };
    row("127.0.0.1", false);
    row("127.255.255.254", false);
    row("0.0.0.0", false);
    row("0.1.2.3", false);
    row("10.255.255.255", false);
    row("169.254.169.254", false);
    row("255.255.255.255", false);
    // Range edges: the address inside the block and its outside neighbour.
    row("100.63.255.255", true);
    row("100.64.0.0", false);
    row("100.127.255.255", false);
    row("100.128.0.0", true);
    row("172.15.255.255", true);
    row("172.16.0.0", false);
    row("172.31.255.255", false);
    row("172.32.0.0", true);
    row("192.167.255.255", true);
    row("192.168.0.0", false);
    row("192.168.255.255", false);
    row("192.169.0.0", true);
    row("198.17.255.255", true);
    row("198.18.0.0", false);
    row("198.19.255.255", false);
    row("198.20.0.0", true);
    row("8.8.8.8", true);
    row("::1", false);
    row("::", false);
    row("fe80::1", false);
    row("febf::1", false);
    row("fc00::1", false);
    row("fdff:ffff::1", false);
    row("2606:4700::1111", true);
    // IPv4-mapped IPv6 must be judged as the IPv4 address it carries.
    row("::ffff:127.0.0.1", false);
    row("::ffff:169.254.169.254", false);
    row("::ffff:172.16.0.1", false);
    row("::ffff:8.8.8.8", true);
}

void MarkdownNetworkPolicyTest::classifiesResolvedAddresses() {
    QFETCH(QString, address);
    QFETCH(bool, isPublic);
    const QHostAddress host(address);
    QVERIFY2(!host.isNull(), qPrintable(address));
    QCOMPARE(MarkdownNetworkPolicy::isPublicAddress(host), isPublic);
}

QTEST_GUILESS_MAIN(MarkdownNetworkPolicyTest)
#include "markdown_network_policy_test.moc"
