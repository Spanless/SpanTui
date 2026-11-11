#pragma once

#include <QElapsedTimer>
#include <QHash>
#include <QMetaObject>
#include <QObject>
#include <QPointer>
#include <QTimer>

#include <optional>

namespace span::remote {

class RemoteShellChannel;

// One reading of a Linux host's kernel counters. CPU and network fields are
// running totals; rates come from the difference of two readings. Fields the
// host does not report stay -1.
struct RemoteSystemSample {
    quint64 cpuTotal = 0;
    quint64 cpuIdle = 0;
    qint64 memoryTotalBytes = -1;
    qint64 memoryAvailableBytes = -1;
    qint64 diskTotalBytes = -1;
    qint64 diskUsedBytes = -1;
    qint64 rxBytes = -1;
    qint64 txBytes = -1;
    QString loadAverage;
};

// The POSIX shell script that prints one reading from /proc and `df`; exits 3
// when the host has no /proc/stat (not Linux).
QString remoteSystemSampleScript();
// Parses the script's output; nullopt when the CPU line is missing.
std::optional<RemoteSystemSample> parseRemoteSystemSample(const QByteArray &payload);

// Resource usage of the server behind a remote terminal: CPU, memory, the
// root filesystem and network throughput, polled through the host's shell
// channel (shared with the file browser). Polls only while some view shows
// the figures (setConsumerVisible), one request in flight at a time, and
// never reconnects a failed channel — the file browser's Reconnect does.
class RemoteSystemMonitor final : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool available READ available NOTIFY sampleChanged FINAL)
    Q_PROPERTY(double cpuPercent READ cpuPercent NOTIFY sampleChanged FINAL)
    Q_PROPERTY(qint64 memoryUsedBytes READ memoryUsedBytes NOTIFY sampleChanged FINAL)
    Q_PROPERTY(qint64 memoryTotalBytes READ memoryTotalBytes NOTIFY sampleChanged FINAL)
    Q_PROPERTY(qint64 diskUsedBytes READ diskUsedBytes NOTIFY sampleChanged FINAL)
    Q_PROPERTY(qint64 diskTotalBytes READ diskTotalBytes NOTIFY sampleChanged FINAL)
    Q_PROPERTY(bool networkAvailable READ networkAvailable NOTIFY sampleChanged FINAL)
    Q_PROPERTY(double rxBytesPerSecond READ rxBytesPerSecond NOTIFY sampleChanged FINAL)
    Q_PROPERTY(double txBytesPerSecond READ txBytesPerSecond NOTIFY sampleChanged FINAL)
    Q_PROPERTY(QString loadAverage READ loadAverage NOTIFY sampleChanged FINAL)

public:
    explicit RemoteSystemMonitor(RemoteShellChannel *channel, QObject *parent = nullptr);
    ~RemoteSystemMonitor() override;

    bool available() const { return m_available; }
    double cpuPercent() const { return m_cpuPercent; }
    qint64 memoryUsedBytes() const;
    qint64 memoryTotalBytes() const { return m_current.memoryTotalBytes; }
    qint64 diskUsedBytes() const { return m_current.diskUsedBytes; }
    qint64 diskTotalBytes() const { return m_current.diskTotalBytes; }
    bool networkAvailable() const { return m_networkAvailable; }
    double rxBytesPerSecond() const { return m_rxRate; }
    double txBytesPerSecond() const { return m_txRate; }
    QString loadAverage() const { return m_current.loadAverage; }

    bool active() const { return m_timer.isActive(); }
    void setInterval(int milliseconds);
    Q_INVOKABLE void setConsumerVisible(QObject *consumer, bool visible);

signals:
    void sampleChanged();

private:
    void setActive(bool active);
    void poll();
    void handleReply(quint64 id, int exitCode, const QByteArray &payload);
    void handleFailure(quint64 id);
    void handleChannelState();
    void clear();

    QPointer<RemoteShellChannel> m_channel;
    QHash<QObject *, QMetaObject::Connection> m_visibleConsumers;
    QTimer m_timer;
    QElapsedTimer m_sampleClock;
    quint64 m_pending = 0;
    bool m_unsupported = false;
    bool m_available = false;
    bool m_networkAvailable = false;
    std::optional<RemoteSystemSample> m_previous;
    RemoteSystemSample m_current;
    double m_cpuPercent = 0;
    double m_rxRate = 0;
    double m_txRate = 0;
};

} // namespace span::remote
