#include "core/external_tools.h"
#include "remote/tailscale_devices.h"

#include <QDir>
#include <QFile>
#include <QHostAddress>
#include <QSignalSpy>
#include <QTcpServer>
#include <QTemporaryDir>
#include <QTest>

using namespace span::remote;
using span::core::ExternalTool;

class TailscaleDevicesTest final : public QObject {
    Q_OBJECT

private slots:
    void cleanup();
    void parsesStatusJson();
    void rejectsOtherOutput();
    void listsDevicesThatAnswerSsh();
    void reportsAMissingCli();
    void reportsCliFailures();
};

namespace {

// Shape of `tailscale status --json` (1.104), trimmed to the fields read.
// Addresses are loopback so the SSH probe stays on this machine: alpha's
// port answers, beta's does not, gamma runs Tailscale SSH, delta is offline
// and the Mullvad exit node is never listed.
const QByteArray kStatus = R"({
  "Version": "1.104.2",
  "BackendState": "Running",
  "Self": { "ID": "n0", "HostName": "workstation", "DNSName": "workstation.tail1234.ts.net.",
            "OS": "windows", "Online": true, "TailscaleIPs": ["100.64.0.1"] },
  "CurrentTailnet": { "Name": "example.github", "MagicDNSSuffix": "tail1234.ts.net" },
  "Peer": {
    "nodekey:d": { "ID": "n4", "HostName": "delta", "DNSName": "delta.tail1234.ts.net.",
                   "OS": "macOS", "Online": false, "TailscaleIPs": ["127.0.0.4"] },
    "nodekey:g": { "ID": "n3", "HostName": "gamma", "DNSName": "gamma.tail1234.ts.net.",
                   "OS": "linux", "Online": true,
                   "TailscaleIPs": ["fd7a:115c:a1e0::3", "127.0.0.3"],
                   "sshHostKeys": ["ssh-ed25519 AAAAC3Nza"] },
    "nodekey:b": { "ID": "n2", "HostName": "beta", "DNSName": "beta.tail1234.ts.net.",
                   "OS": "windows", "Online": true, "TailscaleIPs": ["127.0.0.2"] },
    "nodekey:a": { "ID": "n1", "HostName": "ip-172-26-1-59", "DNSName": "alpha.tail1234.ts.net.",
                   "OS": "linux", "Online": true, "TailscaleIPs": ["127.0.0.1"] },
    "nodekey:m": { "ID": "n5", "HostName": "us-nyc-wg-301.mullvad.ts.net",
                   "OS": "linux", "Online": true, "TailscaleIPs": ["100.80.0.9"],
                   "Location": { "Country": "USA", "City": "New York" } }
  }
})";

// A stand-in tailscale.exe: a batch file that prints `output` and exits
// with `exitCode`, whatever arguments it gets.
QString writeFakeCli(const QTemporaryDir &dir, const QByteArray &output, int exitCode)
{
    QFile data(dir.filePath(QStringLiteral("output.txt")));
    if (!data.open(QIODevice::WriteOnly) || data.write(output) != output.size()) {
        return {};
    }
    data.close();
    const QString path = dir.filePath(QStringLiteral("tailscale.cmd"));
    QFile script(path);
    if (!script.open(QIODevice::WriteOnly)) {
        return {};
    }
    script.write(QByteArrayLiteral("@type \"%~dp0output.txt\"\r\n@exit /b ") + QByteArray::number(exitCode) + "\r\n");
    return QDir::toNativeSeparators(path);
}

QStringList column(const TailscaleDeviceModel &model, int role)
{
    QStringList values;
    for (int row = 0; row < model.rowCount(); ++row) {
        values << model.data(model.index(row), role).toString();
    }
    return values;
}

} // namespace

void TailscaleDevicesTest::cleanup()
{
    span::core::setToolExecutable(ExternalTool::Tailscale, QString());
}

void TailscaleDevicesTest::parsesStatusJson()
{
    const auto status = parseTailscaleStatus(kStatus);
    QVERIFY(status.has_value());
    QCOMPARE(status->backendState, QStringLiteral("Running"));
    QCOMPARE(status->tailnet, QStringLiteral("example.github"));
    QCOMPARE(status->selfName, QStringLiteral("workstation"));
    QCOMPARE(status->peers.size(), 4);

    const auto peer = [&](const QString &name) {
        for (const TailscalePeer &candidate : status->peers) {
            if (candidate.machineName == name) {
                return candidate;
            }
        }
        return TailscalePeer{};
    };
    const TailscalePeer gamma = peer(QStringLiteral("gamma"));
    QCOMPARE(gamma.id, QStringLiteral("n3"));
    QCOMPARE(gamma.dnsName, QStringLiteral("gamma.tail1234.ts.net"));
    QCOMPARE(gamma.os, QStringLiteral("linux"));
    QVERIFY(gamma.online);
    QVERIFY(gamma.tailscaleSsh);
    // IPv4 first: the address a saved host uses.
    QCOMPARE(gamma.addresses, QStringList({QStringLiteral("127.0.0.3"), QStringLiteral("fd7a:115c:a1e0::3")}));
    // Named by the tailnet's machine name, not the OS host name.
    const TailscalePeer alpha = peer(QStringLiteral("alpha"));
    QCOMPARE(alpha.hostName, QStringLiteral("ip-172-26-1-59"));
    QVERIFY(!alpha.tailscaleSsh);
    QVERIFY(!peer(QStringLiteral("delta")).online);
    QVERIFY(peer(QStringLiteral("us-nyc-wg-301.mullvad.ts.net")).id.isEmpty());
}

void TailscaleDevicesTest::rejectsOtherOutput()
{
    QVERIFY(!parseTailscaleStatus("failed to connect to local tailscaled\r\n").has_value());
    QVERIFY(!parseTailscaleStatus(R"({"Peer": {}})").has_value());
    QVERIFY(!parseTailscaleStatus(R"([1, 2])").has_value());

    // A signed-out client: no tailnet, no peers.
    const auto status = parseTailscaleStatus(
        R"({"BackendState": "NeedsLogin", "Self": {"HostName": "workstation"},
            "CurrentTailnet": null, "Peer": null})");
    QVERIFY(status.has_value());
    QCOMPARE(status->backendState, QStringLiteral("NeedsLogin"));
    QVERIFY(status->tailnet.isEmpty());
    QVERIFY(status->peers.isEmpty());
    // Without a MagicDNS name the OS host name stands in.
    QCOMPARE(status->selfName, QStringLiteral("workstation"));
}

void TailscaleDevicesTest::listsDevicesThatAnswerSsh()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString cli = writeFakeCli(dir, kStatus, 0);
    QVERIFY(!cli.isEmpty());
    span::core::setToolExecutable(ExternalTool::Tailscale, cli);
    QTcpServer sshd;
    QVERIFY(sshd.listen(QHostAddress::LocalHost));

    TailscaleDeviceModel model;
    model.setProbePort(sshd.serverPort());
    QCOMPARE(model.state(), QStringLiteral("idle"));
    QVERIFY(!model.refreshing());
    QSignalSpy refreshing(&model, &TailscaleDeviceModel::refreshingChanged);
    model.refresh();
    QVERIFY(model.refreshing());
    QTRY_VERIFY_WITH_TIMEOUT(!model.refreshing(), 15000);
    QCOMPARE(refreshing.count(), 2);

    QCOMPARE(model.state(), QStringLiteral("running"));
    QCOMPARE(model.tailnet(), QStringLiteral("example.github"));
    QCOMPARE(model.deviceCount(), 4);
    QCOMPARE(model.probingCount(), 0);
    // Only devices that accept SSH, by name.
    QCOMPARE(column(model, TailscaleDeviceModel::NameRole),
             QStringList({QStringLiteral("alpha"), QStringLiteral("gamma")}));
    QCOMPARE(column(model, TailscaleDeviceModel::SshStateRole),
             QStringList({QStringLiteral("open"), QStringLiteral("tailscale")}));
    QCOMPARE(column(model, TailscaleDeviceModel::AddressRole),
             QStringList({QStringLiteral("127.0.0.1"), QStringLiteral("127.0.0.3")}));

    // Without the filter: online devices first, offline last.
    model.setSshOnly(false);
    QCOMPARE(column(model, TailscaleDeviceModel::NameRole),
             QStringList(
                 {QStringLiteral("alpha"), QStringLiteral("beta"), QStringLiteral("gamma"), QStringLiteral("delta")}));
    QCOMPARE(column(model, TailscaleDeviceModel::SshStateRole),
             QStringList({QStringLiteral("open"), QStringLiteral("closed"), QStringLiteral("tailscale"),
                          QStringLiteral("offline")}));
}

void TailscaleDevicesTest::reportsAMissingCli()
{
    span::core::setToolExecutable(ExternalTool::Tailscale, QString());
    TailscaleDeviceModel model;
    QSignalSpy status(&model, &TailscaleDeviceModel::statusChanged);
    model.refresh();
    QCOMPARE(status.count(), 1);
    QCOMPARE(model.state(), QStringLiteral("unavailable"));
    QVERIFY(!model.refreshing());
    QCOMPARE(model.rowCount(), 0);
}

void TailscaleDevicesTest::reportsCliFailures()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString cli = writeFakeCli(dir, "failed to connect to local tailscaled\r\n", 1);
    QVERIFY(!cli.isEmpty());
    span::core::setToolExecutable(ExternalTool::Tailscale, cli);

    TailscaleDeviceModel model;
    model.refresh();
    QTRY_VERIFY_WITH_TIMEOUT(!model.refreshing(), 10000);
    QCOMPARE(model.state(), QStringLiteral("error"));
    QCOMPARE(model.error(), QStringLiteral("failed to connect to local tailscaled"));
    QCOMPARE(model.rowCount(), 0);

    // A Windows service no client has brought up idles in NoState: that is
    // disconnected, not starting.
    QTemporaryDir idleDir;
    QVERIFY(idleDir.isValid());
    const QString idleCli =
        writeFakeCli(idleDir, R"({"BackendState": "NoState", "Self": {"HostName": "workstation"}})", 0);
    QVERIFY(!idleCli.isEmpty());
    span::core::setToolExecutable(ExternalTool::Tailscale, idleCli);
    model.refresh();
    QTRY_VERIFY_WITH_TIMEOUT(!model.refreshing(), 10000);
    QCOMPARE(model.state(), QStringLiteral("stopped"));
}

QTEST_GUILESS_MAIN(TailscaleDevicesTest)
#include "tailscale_devices_test.moc"
