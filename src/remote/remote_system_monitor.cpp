#include "remote/remote_system_monitor.h"

#include "remote/remote_shell_channel.h"

#include <QList>

#include <algorithm>

namespace span::remote {
namespace {

constexpr int kDefaultIntervalMs = 3000;
constexpr int kUnsupportedExitCode = 3;

// Virtual links that only mirror traffic already counted on the real
// interface (loopback, container bridges and their veth pairs).
bool countedInterface(const QByteArray &name)
{
    static const QList<QByteArray> skipped{"lo", "veth", "docker", "br-", "virbr"};
    return std::none_of(skipped.cbegin(), skipped.cend(), [&name](const QByteArray &prefix) {
        return prefix == "lo" ? name == prefix : name.startsWith(prefix);
    });
}

qint64 kibibytes(const QByteArray &field)
{
    bool ok = false;
    const qint64 value = field.toLongLong(&ok);
    return ok && value >= 0 ? value * 1024 : -1;
}

} // namespace

QString remoteSystemSampleScript()
{
    // Only /proc/stat is required; every other section may be missing (no
    // MemAvailable before Linux 3.14, no /proc/net/dev in some containers).
    return QStringLiteral("[ -r /proc/stat ] || exit 3\n"
                          "head -n 1 /proc/stat\n"
                          "grep -E '^(MemTotal|MemFree|MemAvailable|Buffers|Cached):' /proc/meminfo 2>/dev/null\n"
                          "[ -r /proc/net/dev ] && tail -n +3 /proc/net/dev | sed 's/^/net /'\n"
                          "df -Pk / 2>/dev/null | tail -n 1 | sed 's/^/df /'\n"
                          "[ -r /proc/loadavg ] && sed 's/^/load /' /proc/loadavg\n"
                          "true");
}

std::optional<RemoteSystemSample> parseRemoteSystemSample(const QByteArray &payload)
{
    RemoteSystemSample sample;
    bool cpuSeen = false;
    QHash<QByteArray, qint64> memory;
    qint64 rx = 0;
    qint64 tx = 0;
    bool networkSeen = false;
    for (const QByteArray &rawLine : payload.split('\n')) {
        const QByteArray line = rawLine.simplified();
        const QList<QByteArray> fields = line.split(' ');
        const QByteArray &tag = fields.first();
        if (tag == "cpu" && fields.size() >= 5) {
            // user nice system idle [iowait irq softirq steal]; guest time is
            // already part of user.
            quint64 total = 0;
            for (qsizetype i = 1; i < std::min<qsizetype>(fields.size(), 9); ++i) {
                total += fields.at(i).toULongLong();
            }
            sample.cpuTotal = total;
            sample.cpuIdle = fields.at(4).toULongLong() + (fields.size() > 5 ? fields.at(5).toULongLong() : 0);
            cpuSeen = true;
        } else if (fields.size() >= 2
                   && (tag == "MemTotal:" || tag == "MemFree:" || tag == "MemAvailable:" || tag == "Buffers:"
                       || tag == "Cached:")) {
            memory.insert(tag.chopped(1), kibibytes(fields.at(1)));
        } else if (tag == "net") {
            const QByteArray rest = line.mid(4);
            const qsizetype colon = rest.indexOf(':');
            if (colon <= 0) continue;
            const QByteArray name = rest.left(colon).trimmed();
            const QList<QByteArray> counters = rest.mid(colon + 1).simplified().split(' ');
            if (counters.size() < 9 || !countedInterface(name)) continue;
            rx += counters.at(0).toLongLong();
            tx += counters.at(8).toLongLong();
            networkSeen = true;
        } else if (tag == "df" && fields.size() >= 7) {
            // POSIX df: filesystem total used available capacity mount; read
            // from the end, the filesystem name may contain spaces.
            const qsizetype n = fields.size();
            sample.diskTotalBytes = kibibytes(fields.at(n - 5));
            sample.diskUsedBytes = kibibytes(fields.at(n - 4));
        } else if (tag == "load" && fields.size() >= 4) {
            sample.loadAverage = QString::fromLatin1(fields.mid(1, 3).join(' '));
        }
    }
    if (!cpuSeen) return std::nullopt;
    sample.memoryTotalBytes = memory.value("MemTotal", -1);
    if (memory.contains("MemAvailable")) {
        sample.memoryAvailableBytes = memory.value("MemAvailable");
    } else if (memory.value("MemFree", -1) >= 0) {
        sample.memoryAvailableBytes = memory.value("MemFree") + std::max<qint64>(0, memory.value("Buffers"))
            + std::max<qint64>(0, memory.value("Cached"));
    }
    if (networkSeen) {
        sample.rxBytes = rx;
        sample.txBytes = tx;
    }
    return sample;
}

RemoteSystemMonitor::RemoteSystemMonitor(RemoteShellChannel *channel, QObject *parent)
    : QObject(parent),
      m_channel(channel)
{
    m_timer.setInterval(kDefaultIntervalMs);
    connect(&m_timer, &QTimer::timeout, this, &RemoteSystemMonitor::poll);
    if (channel) {
        connect(channel, &RemoteShellChannel::replied, this, &RemoteSystemMonitor::handleReply);
        connect(channel, &RemoteShellChannel::requestFailed, this,
                [this](quint64 id, const QString &) { handleFailure(id); });
        connect(channel, &RemoteShellChannel::stateChanged, this, &RemoteSystemMonitor::handleChannelState);
    }
}

RemoteSystemMonitor::~RemoteSystemMonitor() = default;

qint64 RemoteSystemMonitor::memoryUsedBytes() const
{
    if (m_current.memoryTotalBytes < 0 || m_current.memoryAvailableBytes < 0) return -1;
    return std::max<qint64>(0, m_current.memoryTotalBytes - m_current.memoryAvailableBytes);
}

void RemoteSystemMonitor::setInterval(int milliseconds)
{
    m_timer.setInterval(std::max(1, milliseconds));
}

void RemoteSystemMonitor::setConsumerVisible(QObject *consumer, bool visible)
{
    if (!consumer) {
        return;
    }
    if (visible && !m_visibleConsumers.contains(consumer)) {
        m_visibleConsumers.insert(consumer, connect(consumer, &QObject::destroyed, this, [this](QObject *gone) {
                                      m_visibleConsumers.remove(gone);
                                      setActive(!m_visibleConsumers.isEmpty());
                                  }));
    } else if (!visible) {
        disconnect(m_visibleConsumers.take(consumer));
    }
    setActive(!m_visibleConsumers.isEmpty());
}

void RemoteSystemMonitor::setActive(bool active)
{
    if (active == m_timer.isActive()) {
        return;
    }
    if (!active) {
        m_timer.stop();
        // A reading still in flight lands after the view hid: drop it.
        m_pending = 0;
        return;
    }
    // Rates across the hidden period would average over it; start over.
    m_previous.reset();
    m_sampleClock.invalidate();
    m_timer.start();
    poll();
}

void RemoteSystemMonitor::poll()
{
    // A failed channel stays down until the user reconnects it; polling it
    // would relaunch ssh (and its password prompt) every interval.
    if (!m_channel || m_pending != 0 || m_unsupported || m_channel->state() == RemoteShellChannel::State::Failed) {
        return;
    }
    m_pending = m_channel->submit(remoteSystemSampleScript());
}

void RemoteSystemMonitor::handleReply(quint64 id, int exitCode, const QByteArray &payload)
{
    if (id != m_pending) {
        return;
    }
    m_pending = 0;
    const std::optional<RemoteSystemSample> sample = exitCode == 0 ? parseRemoteSystemSample(payload) : std::nullopt;
    if (!sample) {
        // Not Linux (or no usable /proc): stop asking this connection.
        m_unsupported = true;
        clear();
        return;
    }
    const qint64 elapsedMs = m_sampleClock.isValid() ? m_sampleClock.restart() : 0;
    if (!m_sampleClock.isValid()) m_sampleClock.start();
    m_cpuPercent = 0;
    m_rxRate = 0;
    m_txRate = 0;
    if (m_previous && sample->cpuTotal >= m_previous->cpuTotal) {
        const quint64 total = sample->cpuTotal - m_previous->cpuTotal;
        const quint64 idle = std::min(total, sample->cpuIdle - std::min(sample->cpuIdle, m_previous->cpuIdle));
        m_cpuPercent = total > 0 ? 100.0 * double(total - idle) / double(total) : 0;
    }
    if (m_previous && elapsedMs > 0 && sample->rxBytes >= 0 && m_previous->rxBytes >= 0) {
        const double seconds = double(elapsedMs) / 1000.0;
        m_rxRate = double(std::max<qint64>(0, sample->rxBytes - m_previous->rxBytes)) / seconds;
        m_txRate = double(std::max<qint64>(0, sample->txBytes - m_previous->txBytes)) / seconds;
    }
    m_previous = sample;
    m_current = *sample;
    m_available = true;
    m_networkAvailable = sample->rxBytes >= 0;
    emit sampleChanged();
}

void RemoteSystemMonitor::handleFailure(quint64 id)
{
    if (id != m_pending) {
        return;
    }
    m_pending = 0;
    clear();
}

void RemoteSystemMonitor::handleChannelState()
{
    if (!m_channel) {
        return;
    }
    switch (m_channel->state()) {
    case RemoteShellChannel::State::Idle:
        // Stopped by an endpoint change: the next connection may reach
        // another server.
        m_unsupported = false;
        m_pending = 0;
        clear();
        break;
    case RemoteShellChannel::State::Failed:
        m_pending = 0;
        clear();
        break;
    case RemoteShellChannel::State::Connecting:
    case RemoteShellChannel::State::Ready: break;
    }
}

void RemoteSystemMonitor::clear()
{
    m_previous.reset();
    m_sampleClock.invalidate();
    if (!m_available) {
        return;
    }
    m_current = {};
    m_cpuPercent = 0;
    m_rxRate = 0;
    m_txRate = 0;
    m_available = false;
    m_networkAvailable = false;
    emit sampleChanged();
}

} // namespace span::remote
