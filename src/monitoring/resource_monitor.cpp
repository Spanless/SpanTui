#include "monitoring/resource_monitor.h"

#include "monitoring/platform_resource_sampler.h"

#include <QTimer>

#include <condition_variable>
#include <mutex>
#include <thread>
#include <utility>

namespace {
// Polls the sampling thread's result.
constexpr int kCompletionPollMs = 50;
} // namespace

struct ResourceMonitor::CallbackState {
    std::shared_ptr<const ResourceSampleProvider::Result> completed;
};

struct ResourceMonitor::SampleLane {
    std::mutex mutex;
    std::condition_variable changed;
    std::shared_ptr<ResourceSampleProvider> provider;
    std::shared_ptr<CallbackState> callbackState;
    QVector<ResourceSampleProvider::SessionTarget> targets;
    QHash<QString, ResourceSampleProvider::SessionSample> previousSessions;
    qint64 elapsedMs = 0;
    bool hasRequest = false;
    bool stop = false;
    std::thread worker;

    ~SampleLane()
    {
        if (worker.joinable()) {
            worker.detach();
        }
    }
};

ResourceMonitor::ResourceMonitor(SessionTargetSource targetSource, QObject *parent,
                                 std::shared_ptr<ResourceSampleProvider> sampleProvider)
    : QObject(parent),
      m_targetSource(std::move(targetSource)),
      m_timer(new QTimer(this)),
      m_completionTimer(new QTimer(this)),
      m_sampleProvider(sampleProvider ? std::move(sampleProvider) : createPlatformResourceSampleProvider()),
      m_callbackState(std::make_shared<CallbackState>()),
      m_sampleLane(std::make_shared<SampleLane>())
{
    m_physicalMemoryBytes = installedPhysicalMemoryBytes();
    m_sampleLane->provider = m_sampleProvider;
    m_sampleLane->callbackState = m_callbackState;
    const auto lane = m_sampleLane;
    m_sampleLane->worker = std::thread([lane]() {
        while (true) {
            std::unique_lock<std::mutex> lock(lane->mutex);
            lane->changed.wait(lock, [lane]() {
                return lane->stop || lane->hasRequest;
            });
            if (lane->stop) {
                lane->hasRequest = false;
                break;
            }
            auto provider = lane->provider;
            auto callbackState = lane->callbackState;
            auto targets = std::move(lane->targets);
            auto previousSessions = std::move(lane->previousSessions);
            const qint64 elapsedMs = lane->elapsedMs;
            lane->hasRequest = false;
            lock.unlock();

            ResourceSampleProvider::Result sampled;
            try {
                sampled = provider->sample(
                    targets, previousSessions, elapsedMs);
            } catch (...) {
                sampled.gpuCounterStatus = ResourceSampleProvider::GpuCounterStatus::ProviderFailed;
            }
            const auto result = std::make_shared<
                const ResourceSampleProvider::Result>(std::move(sampled));
            std::atomic_store_explicit(
                &callbackState->completed, result, std::memory_order_release);
        }
        lane->provider.reset();
    });
    m_timer->setTimerType(Qt::CoarseTimer);
    connect(m_timer, &QTimer::timeout, this, &ResourceMonitor::refresh);
    m_completionTimer->setInterval(kCompletionPollMs);
    m_completionTimer->setTimerType(Qt::CoarseTimer);
    connect(m_completionTimer, &QTimer::timeout,
            this, &ResourceMonitor::pollSampleCompletion);
    m_elapsed.start();
    // Inactive until a visible consumer asks for metrics.
    applyEffectiveInterval();
}

ResourceMonitor::~ResourceMonitor() {
    m_timer->stop();
    m_completionTimer->stop();
    if (!m_sampleLane) {
        return;
    }
    {
        std::lock_guard<std::mutex> lock(m_sampleLane->mutex);
        m_sampleLane->stop = true;
    }
    m_sampleLane->changed.notify_one();
    if (m_sampleLane->worker.joinable()) {
        m_sampleLane->worker.detach();
    }
    m_sampleLane.reset();
}

double ResourceMonitor::cpuPercent() const { return m_cpuPercent; }
double ResourceMonitor::gpuPercent() const { return m_gpuPercent; }

int ResourceMonitor::effectiveIntervalMs() const {
    return m_intervalSeconds * 1000 * (m_idle ? 5 : 1);
}

void ResourceMonitor::applyEffectiveInterval() {
    m_timer->setInterval(effectiveIntervalMs());
    if (m_active) m_timer->start();
}

void ResourceMonitor::setIntervalSeconds(int seconds) {
    seconds = qBound(1, seconds, 60);
    if (m_intervalSeconds == seconds) {
        return;
    }
    m_intervalSeconds = seconds;
    applyEffectiveInterval();
}

void ResourceMonitor::setActive(bool active)
{
    if (m_active == active) {
        return;
    }
    m_active = active;
    if (!m_active) {
        m_timer->stop();
        return;
    }
    refresh();
    applyEffectiveInterval();
}

void ResourceMonitor::setConsumerVisible(QObject *consumer, bool visible)
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

void ResourceMonitor::setIdle(bool idle) {
    if (m_idle == idle) {
        return;
    }
    m_idle = idle;
    emit idleChanged();
    if (!m_idle && m_active) {
        // Waking resamples at once (still bounded by the single-in-flight
        // guard) so the meter does not show a stale idle-period value.
        refresh();
    }
    applyEffectiveInterval();
}

QVariantMap ResourceMonitor::sessionMetrics(const QString &sessionId) const {
    QVariantMap metrics;
    // Every target of a sampling round has an entry (stopped sessions an
    // invalid one), so an unknown id yields no metrics.
    const auto found = m_sessionSamples.constFind(sessionId);
    if (found == m_sessionSamples.constEnd()) {
        return metrics;
    }
    const ResourceSampleProvider::SessionSample &sample = *found;
    metrics.insert(QStringLiteral("cpuPercent"), sample.valid ? sample.cpuPercent : 0.0);
    metrics.insert(QStringLiteral("privateBytes"), sample.valid ? sample.privateBytes : qint64(0));
    metrics.insert(QStringLiteral("workingSetBytes"), sample.valid ? sample.workingSetBytes : qint64(0));
    // Prefer working set for UI "memory" — matches Task Manager's default column.
    metrics.insert(QStringLiteral("gpuPercent"), sample.valid ? sample.gpuPercent : 0.0);
    metrics.insert(QStringLiteral("memoryBytes"),
                   sample.valid ? sample.workingSetBytes : qint64(0));
    return metrics;
}

void ResourceMonitor::refresh() {
    if (m_samplingInProgress || !m_targetSource || !m_sampleProvider) {
        return;
    }
    const qint64 elapsedMs = m_elapsed.restart();
    m_samplingInProgress = true;
    QVector<ResourceSampleProvider::SessionTarget> targets = m_targetSource();
    const auto previousSamples = m_sessionSamples;
    m_completionTimer->start();
    {
        std::lock_guard<std::mutex> lock(m_sampleLane->mutex);
        if (m_sampleLane->stop) {
            m_samplingInProgress = false;
            m_completionTimer->stop();
            return;
        }
        m_sampleLane->targets = std::move(targets);
        m_sampleLane->previousSessions = previousSamples;
        m_sampleLane->elapsedMs = elapsedMs;
        m_sampleLane->hasRequest = true;
    }
    m_sampleLane->changed.notify_one();
}

void ResourceMonitor::pollSampleCompletion()
{
    const auto result = std::atomic_exchange_explicit(
        &m_callbackState->completed,
        std::shared_ptr<const ResourceSampleProvider::Result>{},
        std::memory_order_acq_rel);
    if (!result) {
        return;
    }
    m_completionTimer->stop();
    applySample(*result);
}

void ResourceMonitor::applySample(const ResourceSampleProvider::Result &sample)
{
    m_sessionSamples = sample.sessions;
    m_gpuCounterStatus = sample.gpuCounterStatus;
    m_cpuPercent = sample.cpuPercent;
    m_gpuPercent = sample.gpuPercent;
    m_samplingInProgress = false;
    emit metricsChanged();
}
