#pragma once

#include <QObject>
#include <QElapsedTimer>
#include <QHash>
#include <QList>
#include <QVariantMap>
#include <QVector>

#include <atomic>
#include <functional>
#include <memory>

class QTimer;

class ResourceSampleProvider {
public:
    enum class PdhStatus : int {
        NotInitialized = 0,
        Ready = 1,
        QueryFailed = 2,
        ProviderFailed = 3,
    };

    struct SessionSample {
        QHash<quint32, qint64> lastCpuByPid;
        double cpuPercent = 0;
        double gpuPercent = 0;
        qint64 privateBytes = 0;
        qint64 workingSetBytes = 0;
        bool valid = false;
    };

    struct SessionTarget {
        QString id;
        quint32 rootPid = 0;
        QList<quint32> jobPids;
        bool running = false;
    };

    struct Result {
        QHash<QString, SessionSample> sessions;
        double cpuPercent = 0;
        double gpuPercent = 0;
        PdhStatus pdhStatus = PdhStatus::NotInitialized;
    };

    virtual ~ResourceSampleProvider() = default;
    virtual Result sample(
        const QVector<SessionTarget> &targets,
        const QHash<QString, SessionSample> &previousSessions,
        qint64 elapsedMs) = 0;
};

// Lightweight, in-process resource sampler for the product UI.  It performs
// no allocation tracking and writes nothing to disk; the interval is user
// configurable so it remains a diagnostic instead of a source of idle work.
// It samples only while active: a new monitor is inactive, and visible
// metric consumers switch it on through setConsumerVisible(). While
// IdleGovernor reports the application idle, the effective interval is
// multiplied by 5; waking resamples immediately.
//
// App-wide metrics cover SpanTui itself.  sessionMetrics() also reports the
// session job (CPU %, GPU %, and private memory): the shell, its console host
// and every descendant still assigned to that terminal.
class ResourceMonitor final : public QObject {
    Q_OBJECT
    Q_PROPERTY(double cpuPercent READ cpuPercent NOTIFY metricsChanged FINAL)
    Q_PROPERTY(double gpuPercent READ gpuPercent NOTIFY metricsChanged FINAL)
    Q_PROPERTY(bool idle READ idle NOTIFY idleChanged FINAL)
    // Installed RAM, the scale for a session's memory load level; 0 unknown.
    Q_PROPERTY(qint64 physicalMemoryBytes READ physicalMemoryBytes CONSTANT FINAL)

public:
    // The sessions to measure, asked for on the GUI thread at every sampling
    // round; the host that owns the sessions supplies it.
    using SessionTargetSource = std::function<QVector<ResourceSampleProvider::SessionTarget>()>;

    explicit ResourceMonitor(SessionTargetSource targetSource, QObject *parent = nullptr,
                             std::shared_ptr<ResourceSampleProvider> sampleProvider = {});
    ~ResourceMonitor() override;

    double cpuPercent() const;
    double gpuPercent() const;
    bool idle() const { return m_idle; }
    bool active() const { return m_active; }
    int effectiveIntervalMs() const;
    bool samplingInProgress() const { return m_samplingInProgress; }
    int pdhStatus() const { return static_cast<int>(m_pdhStatus); }

    void setIntervalSeconds(int seconds);
    void setIdle(bool idle);
    // Activation samples at once and starts the timer; deactivation stops
    // the timer (a sample already in flight still lands).
    void setActive(bool active);
    // Each consumer showing metrics registers while visible; the monitor is
    // active while at least one is. A destroyed consumer unregisters itself.
    Q_INVOKABLE void setConsumerVisible(QObject *consumer, bool visible);
    Q_INVOKABLE QVariantMap sessionMetrics(const QString &sessionId) const;
    qint64 physicalMemoryBytes() const { return m_physicalMemoryBytes; }

signals:
    void metricsChanged();
    void idleChanged();

private:
    void refresh();
    void applyEffectiveInterval();
    void pollSampleCompletion();
    void applySample(const ResourceSampleProvider::Result &sample);
    struct CallbackState;
    struct SampleLane;
    SessionTargetSource m_targetSource;
    QTimer *m_timer = nullptr;
    QTimer *m_completionTimer = nullptr;
    QElapsedTimer m_elapsed;
    double m_cpuPercent = 0;
    double m_gpuPercent = 0;
    int m_intervalSeconds = 2;
    bool m_idle = false;
    bool m_active = false;
    QHash<QObject *, QMetaObject::Connection> m_visibleConsumers;
    ResourceSampleProvider::PdhStatus m_pdhStatus =
        ResourceSampleProvider::PdhStatus::NotInitialized;
    QHash<QString, ResourceSampleProvider::SessionSample> m_sessionSamples;
    qint64 m_physicalMemoryBytes = 0;
    bool m_samplingInProgress = false;
    std::shared_ptr<ResourceSampleProvider> m_sampleProvider;
    std::shared_ptr<CallbackState> m_callbackState;
    std::shared_ptr<SampleLane> m_sampleLane;
};
