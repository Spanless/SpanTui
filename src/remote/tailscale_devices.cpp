#include "remote/tailscale_devices.h"

#include "core/core_constants.h"
#include "core/external_tools.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QTcpSocket>
#include <QTimer>

#include <algorithm>

namespace span::remote {
namespace {

constexpr int kQueryTimeoutMs = 8000;
constexpr int kProbeTimeoutMs = 3000;

bool isIpv4(const QString &address)
{
    return !address.contains(QLatin1Char(':'));
}

TailscalePeer peerFrom(const QJsonObject &object)
{
    TailscalePeer peer;
    peer.id = object.value(QStringLiteral("ID")).toString();
    peer.dnsName = object.value(QStringLiteral("DNSName")).toString();
    if (peer.dnsName.endsWith(QLatin1Char('.'))) {
        peer.dnsName.chop(1);
    }
    peer.hostName = object.value(QStringLiteral("HostName")).toString();
    peer.machineName = peer.dnsName.section(QLatin1Char('.'), 0, 0);
    if (peer.machineName.isEmpty()) {
        peer.machineName = peer.hostName;
    }
    peer.os = object.value(QStringLiteral("OS")).toString();
    peer.online = object.value(QStringLiteral("Online")).toBool();
    for (const QJsonValue &address : object.value(QStringLiteral("TailscaleIPs")).toArray()) {
        if (!address.toString().isEmpty()) {
            peer.addresses << address.toString();
        }
    }
    std::stable_partition(peer.addresses.begin(), peer.addresses.end(), isIpv4);
    peer.tailscaleSsh = !object.value(QStringLiteral("sshHostKeys")).toArray().isEmpty();
    return peer;
}

QString stateKey(const QString &backendState)
{
    if (backendState == QLatin1String("Running")) {
        return QStringLiteral("running");
    }
    if (backendState == QLatin1String("NeedsLogin") || backendState == QLatin1String("NeedsMachineAuth")) {
        return QStringLiteral("needsLogin");
    }
    if (backendState == QLatin1String("Starting")) {
        return QStringLiteral("starting");
    }
    // Stopped, and NoState: on Windows the service idles in NoState for as
    // long as no client has connected it, so it is not "still starting".
    return QStringLiteral("stopped");
}

QString firstLine(const QByteArray &text)
{
    for (const QString &line : QString::fromLocal8Bit(text).split(QLatin1Char('\n'))) {
        const QString trimmed = line.trimmed();
        if (!trimmed.isEmpty()) {
            return trimmed;
        }
    }
    return {};
}

} // namespace

std::optional<TailscaleStatus> parseTailscaleStatus(const QByteArray &json)
{
    QJsonParseError error;
    const QJsonDocument document = QJsonDocument::fromJson(json, &error);
    if (error.error != QJsonParseError::NoError || !document.isObject()) {
        return std::nullopt;
    }
    const QJsonObject root = document.object();
    if (!root.value(QStringLiteral("BackendState")).isString()) {
        return std::nullopt;
    }
    TailscaleStatus status;
    status.backendState = root.value(QStringLiteral("BackendState")).toString();
    status.tailnet = root.value(QStringLiteral("CurrentTailnet")).toObject().value(QStringLiteral("Name")).toString();
    status.selfName = peerFrom(root.value(QStringLiteral("Self")).toObject()).machineName;
    const QJsonObject peers = root.value(QStringLiteral("Peer")).toObject();
    for (auto it = peers.begin(); it != peers.end(); ++it) {
        const QJsonObject object = it.value().toObject();
        const QJsonValue location = object.value(QStringLiteral("Location"));
        if (!location.isUndefined() && !location.isNull()) {
            continue;
        }
        status.peers << peerFrom(object);
    }
    return status;
}

TailscaleDeviceModel::TailscaleDeviceModel(QObject *parent) : QAbstractListModel(parent) {}

TailscaleDeviceModel::~TailscaleDeviceModel()
{
    cancelProbes();
    if (m_query) {
        m_query->disconnect(this);
        m_query->kill();
        m_query->waitForFinished(span::core::kProcessKillWaitMs);
    }
}

int TailscaleDeviceModel::rowCount(const QModelIndex &parent) const
{
    return parent.isValid() ? 0 : int(m_rows.size());
}

QVariant TailscaleDeviceModel::data(const QModelIndex &index, int role) const
{
    if (!index.isValid() || index.row() < 0 || index.row() >= m_rows.size()) {
        return {};
    }
    const Device &device = m_devices.at(m_rows.at(index.row()));
    switch (role) {
    case DeviceIdRole: return device.peer.id;
    case NameRole: return device.peer.machineName;
    case HostNameRole: return device.peer.hostName;
    case DnsNameRole: return device.peer.dnsName;
    case AddressRole: return device.peer.addresses.value(0);
    case OsRole: return device.peer.os;
    case OnlineRole: return device.peer.online;
    case SshStateRole:
        switch (device.ssh) {
        case Ssh::Tailscale: return QStringLiteral("tailscale");
        case Ssh::Open: return QStringLiteral("open");
        case Ssh::Closed: return QStringLiteral("closed");
        case Ssh::Probing: return QStringLiteral("probing");
        case Ssh::Offline: return QStringLiteral("offline");
        }
        return {};
    default: return {};
    }
}

QHash<int, QByteArray> TailscaleDeviceModel::roleNames() const
{
    return {
        {DeviceIdRole, "deviceId"}, {NameRole, "name"}, {HostNameRole, "hostName"}, {DnsNameRole, "dnsName"},
        {AddressRole, "address"},   {OsRole, "os"},     {OnlineRole, "online"},     {SshStateRole, "sshState"},
    };
}

int TailscaleDeviceModel::probingCount() const
{
    return int(std::count_if(m_devices.cbegin(), m_devices.cend(),
                             [](const Device &device) { return device.ssh == Ssh::Probing; }));
}

void TailscaleDeviceModel::setSshOnly(bool sshOnly)
{
    if (m_sshOnly == sshOnly) {
        return;
    }
    m_sshOnly = sshOnly;
    beginResetModel();
    rebuildRows();
    endResetModel();
    emit sshOnlyChanged();
}

void TailscaleDeviceModel::refresh()
{
    const bool wasRefreshing = refreshing();
    const QString program = span::core::toolExecutable(span::core::ExternalTool::Tailscale);
    if (program.isEmpty()) {
        ++m_generation;
        cancelProbes();
        if (m_query) {
            m_query->disconnect(this);
            m_query->kill();
            m_query->deleteLater();
            m_query = nullptr;
        }
        beginResetModel();
        m_status = {};
        m_devices.clear();
        m_rows.clear();
        endResetModel();
        emit devicesChanged();
        setState(QStringLiteral("unavailable"));
    } else if (!m_query) {
        // The previous refresh's probes no longer count.
        ++m_generation;
        cancelProbes();
        auto *query = new QProcess(this);
        m_query = query;
        query->setProgram(program);
        query->setArguments({QStringLiteral("status"), QStringLiteral("--json")});
        connect(query, &QProcess::finished, this, [this, query]() { finishQuery(query, true); });
        connect(query, &QProcess::errorOccurred, this, [this, query](QProcess::ProcessError error) {
            if (error == QProcess::FailedToStart) {
                finishQuery(query, false);
            }
        });
        // A hung CLI (service wedged) is killed and then reports as finished.
        QTimer::singleShot(kQueryTimeoutMs, query, [query]() { query->kill(); });
        query->start(QIODevice::ReadOnly);
    }
    if (wasRefreshing != refreshing()) {
        emit refreshingChanged();
    }
}

void TailscaleDeviceModel::finishQuery(QProcess *query, bool ran)
{
    if (m_query != query) {
        return;
    }
    const bool wasRefreshing = refreshing();
    m_query = nullptr;
    query->disconnect(this);
    query->deleteLater();
    const QByteArray output = ran ? query->readAllStandardOutput() : QByteArray();
    const QByteArray errors = ran ? query->readAllStandardError() : QByteArray();
    // The JSON comes with any exit code: a logged-out client still reports
    // its state.
    const std::optional<TailscaleStatus> status = parseTailscaleStatus(output);

    beginResetModel();
    m_status = status.value_or(TailscaleStatus{});
    m_devices.clear();
    if (status && stateKey(status->backendState) == QLatin1String("running")) {
        for (const TailscalePeer &peer : status->peers) {
            Device device;
            device.peer = peer;
            device.ssh = !peer.online ? Ssh::Offline : peer.tailscaleSsh ? Ssh::Tailscale : Ssh::Probing;
            m_devices << device;
        }
        std::stable_sort(m_devices.begin(), m_devices.end(), [](const Device &left, const Device &right) {
            if (left.peer.online != right.peer.online) {
                return left.peer.online;
            }
            return left.peer.machineName.compare(right.peer.machineName, Qt::CaseInsensitive) < 0;
        });
    }
    rebuildRows();
    endResetModel();
    emit devicesChanged();

    if (status) {
        setState(stateKey(status->backendState));
    } else {
        const QString message = firstLine(errors);
        setState(QStringLiteral("error"), message.isEmpty() ? firstLine(output) : message);
    }
    for (int i = 0; i < m_devices.size(); ++i) {
        if (m_devices.at(i).ssh == Ssh::Probing) {
            probe(i);
        }
    }
    if (wasRefreshing != refreshing()) {
        emit refreshingChanged();
    }
}

void TailscaleDeviceModel::setState(const QString &state, const QString &error)
{
    m_state = state;
    m_error = error;
    emit statusChanged();
}

void TailscaleDeviceModel::probe(int deviceIndex)
{
    const QString address = m_devices.at(deviceIndex).peer.addresses.value(0);
    const quint64 generation = m_generation;
    if (address.isEmpty()) {
        QTimer::singleShot(0, this, [this, generation, deviceIndex]() { finishProbe(generation, deviceIndex, false); });
        return;
    }
    m_probes.removeAll(nullptr);
    auto *socket = new QTcpSocket(this);
    m_probes << socket;
    const auto done = [this, socket, generation, deviceIndex](bool open) {
        socket->disconnect(this);
        socket->abort();
        socket->deleteLater();
        finishProbe(generation, deviceIndex, open);
    };
    connect(socket, &QTcpSocket::connected, this, [done]() { done(true); });
    connect(socket, &QTcpSocket::errorOccurred, this, [done]() { done(false); });
    // Filtered ports drop the SYN instead of refusing it.
    QTimer::singleShot(kProbeTimeoutMs, socket, [done]() { done(false); });
    socket->connectToHost(address, m_probePort);
}

void TailscaleDeviceModel::finishProbe(quint64 generation, int deviceIndex, bool open)
{
    if (generation != m_generation || deviceIndex < 0 || deviceIndex >= m_devices.size()
        || m_devices.at(deviceIndex).ssh != Ssh::Probing) {
        return;
    }
    const bool wasRefreshing = refreshing();
    m_devices[deviceIndex].ssh = open ? Ssh::Open : Ssh::Closed;
    const int row = int(m_rows.indexOf(deviceIndex));
    if (row >= 0) {
        if (shown(m_devices.at(deviceIndex))) {
            emit dataChanged(index(row), index(row), {SshStateRole});
        } else {
            beginRemoveRows({}, row, row);
            m_rows.removeAt(row);
            endRemoveRows();
        }
    } else if (shown(m_devices.at(deviceIndex))) {
        // Rows keep the device order, so the new row goes before the first
        // later device.
        const auto later = std::find_if(m_rows.cbegin(), m_rows.cend(),
                                        [deviceIndex](int shownIndex) { return shownIndex > deviceIndex; });
        const int at = int(later - m_rows.cbegin());
        beginInsertRows({}, at, at);
        m_rows.insert(at, deviceIndex);
        endInsertRows();
    }
    emit devicesChanged();
    if (wasRefreshing != refreshing()) {
        emit refreshingChanged();
    }
}

void TailscaleDeviceModel::cancelProbes()
{
    for (const QPointer<QTcpSocket> &socket : std::as_const(m_probes)) {
        if (socket) {
            socket->disconnect(this);
            socket->abort();
            socket->deleteLater();
        }
    }
    m_probes.clear();
}

bool TailscaleDeviceModel::shown(const Device &device) const
{
    return !m_sshOnly || device.ssh == Ssh::Tailscale || device.ssh == Ssh::Open;
}

void TailscaleDeviceModel::rebuildRows()
{
    m_rows.clear();
    for (int i = 0; i < m_devices.size(); ++i) {
        if (shown(m_devices.at(i))) {
            m_rows << i;
        }
    }
}

} // namespace span::remote
