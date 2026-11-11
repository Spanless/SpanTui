#pragma once

#include <QAbstractListModel>
#include <QPointer>
#include <QString>
#include <QStringList>
#include <QVector>

#include <optional>

class QProcess;
class QTcpSocket;

namespace span::remote {

// One other device of the tailnet as `tailscale status --json` lists it.
struct TailscalePeer {
    QString id;
    // The machine name the tailnet knows the device by (admin console, first
    // label of the MagicDNS name); the OS host name when there is none.
    QString machineName;
    // The device's own OS host name.
    QString hostName;
    // MagicDNS name without the trailing dot; empty when the tailnet has none.
    QString dnsName;
    QString os;
    // Tailscale addresses, IPv4 first.
    QStringList addresses;
    bool online = false;
    // The device runs Tailscale SSH (it publishes SSH host keys).
    bool tailscaleSsh = false;
};

struct TailscaleStatus {
    // Tailscale's own words: Running, NeedsLogin, Stopped, Starting, ...
    QString backendState;
    QString tailnet;
    QString selfName;
    QVector<TailscalePeer> peers;
};

// Parses `tailscale status --json`; nullopt when it is not that document.
// Mullvad exit nodes (peers with a Location) are left out: they never accept
// SSH.
std::optional<TailscaleStatus> parseTailscaleStatus(const QByteArray &json);

// The devices of the local tailnet and whether each one accepts SSH, for
// adding them as remote hosts by their Tailscale address — ssh then reaches
// them straight over the tailnet. A device answers SSH when it runs Tailscale
// SSH, or when its SSH port takes a TCP connection over its first Tailscale
// address. Nothing runs until refresh(); each refresh queries the CLI once
// and probes every online device, and results of an older refresh are
// dropped. The `tailscale` executable comes from core::toolExecutable.
class TailscaleDeviceModel final : public QAbstractListModel {
    Q_OBJECT
    // unavailable (no CLI), idle (never refreshed), running, needsLogin,
    // stopped, starting, error.
    Q_PROPERTY(QString state READ state NOTIFY statusChanged FINAL)
    Q_PROPERTY(QString tailnet READ tailnet NOTIFY statusChanged FINAL)
    Q_PROPERTY(QString selfName READ selfName NOTIFY statusChanged FINAL)
    Q_PROPERTY(QString error READ error NOTIFY statusChanged FINAL)
    Q_PROPERTY(bool refreshing READ refreshing NOTIFY refreshingChanged FINAL)
    // Devices of the tailnet, before the SSH filter.
    Q_PROPERTY(int deviceCount READ deviceCount NOTIFY devicesChanged FINAL)
    // Online devices whose SSH probe is still running.
    Q_PROPERTY(int probingCount READ probingCount NOTIFY devicesChanged FINAL)
    // Rows only for devices known to accept SSH (default on).
    Q_PROPERTY(bool sshOnly READ sshOnly WRITE setSshOnly NOTIFY sshOnlyChanged FINAL)

public:
    enum Role {
        DeviceIdRole = Qt::UserRole + 1,
        // Machine name.
        NameRole,
        HostNameRole,
        DnsNameRole,
        AddressRole,
        OsRole,
        OnlineRole,
        // tailscale, open, closed, probing, offline
        SshStateRole,
    };

    explicit TailscaleDeviceModel(QObject *parent = nullptr);
    ~TailscaleDeviceModel() override;

    int rowCount(const QModelIndex &parent = {}) const override;
    QVariant data(const QModelIndex &index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

    QString state() const { return m_state; }
    QString tailnet() const { return m_status.tailnet; }
    QString selfName() const { return m_status.selfName; }
    QString error() const { return m_error; }
    bool refreshing() const { return m_query != nullptr || probingCount() > 0; }
    int deviceCount() const { return int(m_devices.size()); }
    int probingCount() const;
    bool sshOnly() const { return m_sshOnly; }
    void setSshOnly(bool sshOnly);

    // TCP port probed for a plain OpenSSH server (22).
    void setProbePort(quint16 port) { m_probePort = port; }

    Q_INVOKABLE void refresh();

signals:
    void statusChanged();
    void refreshingChanged();
    void devicesChanged();
    void sshOnlyChanged();

private:
    enum class Ssh { Tailscale, Open, Closed, Probing, Offline };
    struct Device {
        TailscalePeer peer;
        Ssh ssh = Ssh::Offline;
    };

    void finishQuery(QProcess *query, bool ran);
    void setState(const QString &state, const QString &error = {});
    void probe(int deviceIndex);
    void finishProbe(quint64 generation, int deviceIndex, bool open);
    void cancelProbes();
    bool shown(const Device &device) const;
    void rebuildRows();

    QString m_state = QStringLiteral("idle");
    QString m_error;
    TailscaleStatus m_status;
    QVector<Device> m_devices;
    // Indices into m_devices of the visible rows, in display order.
    QVector<int> m_rows;
    QPointer<QProcess> m_query;
    QVector<QPointer<QTcpSocket>> m_probes;
    quint64 m_generation = 0;
    quint16 m_probePort = 22;
    bool m_sshOnly = true;
};

} // namespace span::remote
