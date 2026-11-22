#include "platform/directory_change_watcher.h"

#include <QDir>
#include <QFile>
#include <QPointer>

#include <atomic>
#include <cstdlib>

#include <CoreServices/CoreServices.h>
#include <dispatch/dispatch.h>

namespace span::platform {

namespace {
// Coalescing delay of the stream: a burst of writes arrives as one batch.
constexpr CFTimeInterval kLatencySeconds = 0.05;
constexpr FSEventStreamEventFlags kOverflowFlags =
    kFSEventStreamEventFlagMustScanSubDirs | kFSEventStreamEventFlagUserDropped | kFSEventStreamEventFlagKernelDropped;
} // namespace

// Shared with the stream's dispatch queue; a batch carries the generation it
// was produced for, so one that lands after stop() or a new watch() is dropped.
struct WatchContext {
    QPointer<DirectoryChangeWatcher> owner;
    // Canonical root with a trailing '/': the prefix FSEvents paths carry.
    QString prefix;
    std::atomic<quint64> *generation = nullptr;
    quint64 watchGeneration = 0;
};

struct DirectoryChangeWatcher::Impl {
    FSEventStreamRef stream = nullptr;
    dispatch_queue_t queue = dispatch_queue_create("SpanTui.DirectoryChangeWatcher", DISPATCH_QUEUE_SERIAL);
    std::atomic<quint64> generation{0};
    WatchContext *context = nullptr;

    ~Impl() { dispatch_release(queue); }
};

namespace {

void streamCallback(ConstFSEventStreamRef, void *info, size_t count, void *eventPaths,
                    const FSEventStreamEventFlags flags[], const FSEventStreamEventId[])
{
    const auto *context = static_cast<const WatchContext *>(info);
    const auto *paths = static_cast<char **>(eventPaths);
    QStringList relative;
    bool overflow = false;
    bool rootGone = false;
    for (size_t i = 0; i < count; ++i) {
        if (flags[i] & kFSEventStreamEventFlagRootChanged) rootGone = true;
        if (flags[i] & kOverflowFlags) overflow = true;
        const QString path = QFile::decodeName(paths[i]);
        if (path.startsWith(context->prefix)) {
            const QString name = path.mid(context->prefix.size());
            if (!name.isEmpty() && !relative.contains(name)) relative.append(name);
        }
    }
    if (overflow) relative.clear();
    const QPointer<DirectoryChangeWatcher> owner = context->owner;
    if (!owner) return;
    std::atomic<quint64> *generation = context->generation;
    const quint64 expected = context->watchGeneration;
    QMetaObject::invokeMethod(
        owner.data(),
        [owner, generation, expected, relative, rootGone]() {
            if (!owner || generation->load() != expected) return;
            // A watch whose root was deleted or renamed ends, still reporting
            // the change.
            if (rootGone) owner->stop();
            emit owner->changed(relative);
        },
        Qt::QueuedConnection);
}

} // namespace

DirectoryChangeWatcher::DirectoryChangeWatcher(QObject *parent) : QObject(parent), m_impl(std::make_unique<Impl>()) {}

DirectoryChangeWatcher::~DirectoryChangeWatcher()
{
    stop();
}

bool DirectoryChangeWatcher::watch(const QString &root)
{
    stop();
    if (root.isEmpty()) return false;
    char *canonical = realpath(QFile::encodeName(QDir(root).absolutePath()).constData(), nullptr);
    if (!canonical) return false;
    const QString canonicalRoot = QFile::decodeName(canonical);
    std::free(canonical);

    auto *context = new WatchContext;
    context->owner = this;
    context->prefix = canonicalRoot.endsWith(QLatin1Char('/')) ? canonicalRoot : canonicalRoot + QLatin1Char('/');
    context->generation = &m_impl->generation;
    context->watchGeneration = m_impl->generation.load();

    FSEventStreamContext streamContext{};
    streamContext.info = context;
    const QByteArray rootBytes = QFile::encodeName(canonicalRoot);
    CFStringRef rootString = CFStringCreateWithFileSystemRepresentation(kCFAllocatorDefault, rootBytes.constData());
    CFArrayRef roots =
        CFArrayCreate(kCFAllocatorDefault, reinterpret_cast<const void **>(&rootString), 1, &kCFTypeArrayCallBacks);
    m_impl->stream = FSEventStreamCreate(
        kCFAllocatorDefault, streamCallback, &streamContext, roots, kFSEventStreamEventIdSinceNow, kLatencySeconds,
        kFSEventStreamCreateFlagFileEvents | kFSEventStreamCreateFlagNoDefer | kFSEventStreamCreateFlagWatchRoot);
    CFRelease(roots);
    CFRelease(rootString);
    if (!m_impl->stream) {
        delete context;
        return false;
    }
    m_impl->context = context;
    FSEventStreamSetDispatchQueue(m_impl->stream, m_impl->queue);
    if (!FSEventStreamStart(m_impl->stream)) {
        stop();
        return false;
    }
    return true;
}

void DirectoryChangeWatcher::stop()
{
    // Batches already queued for the old watch are dropped on arrival.
    ++m_impl->generation;
    if (m_impl->stream) {
        FSEventStreamStop(m_impl->stream);
        FSEventStreamInvalidate(m_impl->stream);
        FSEventStreamRelease(m_impl->stream);
        m_impl->stream = nullptr;
        // A callback already running on the queue may still read the
        // context; batches it posted copied what they need.
        dispatch_sync(m_impl->queue, ^{});
    }
    delete m_impl->context;
    m_impl->context = nullptr;
}

bool DirectoryChangeWatcher::watching() const
{
    return m_impl->stream != nullptr;
}

} // namespace span::platform
