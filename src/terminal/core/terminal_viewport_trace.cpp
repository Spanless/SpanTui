#include "terminal/core/terminal_viewport_trace.h"
#include "core/product_paths.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QMutex>
#include <QMutexLocker>
#include <QThread>

#include <atomic>

namespace span::terminal {

namespace {

#ifdef QT_NO_DEBUG
std::atomic_bool g_enabled {false};
#else
std::atomic_bool g_enabled {true};
#endif

} // namespace

bool TerminalViewportTrace::enabled()
{
    return g_enabled.load(std::memory_order_relaxed);
}

void TerminalViewportTrace::setEnabled(bool enabled)
{
    g_enabled.store(enabled, std::memory_order_relaxed);
}

void TerminalViewportTrace::record(const QString &session, const QString &stage,
                                  QJsonObject details)
{
    if (!enabled()) return;
    static QMutex mutex;
    QMutexLocker lock(&mutex);
    const QDir directory(span::core::ProductPaths::root()
                         + QStringLiteral("/diagnostics/terminal"));
    if (!directory.mkpath(QStringLiteral("."))) return;
    const QString path = directory.filePath(QStringLiteral("viewport.jsonl"));
    QFile file(path);
    // Bound both the active trace and its single predecessor during long runs.
    if (file.size() >= 2 * 1024 * 1024) {
        const QString previous = directory.filePath(QStringLiteral("viewport.1.jsonl"));
        if (QFile::exists(previous) && !QFile::remove(previous)) return;
        if (!file.rename(previous)) return;
        file.setFileName(path);
    }
    if (!file.open(QIODevice::WriteOnly | QIODevice::Append)) return;
    const QJsonObject event{
        {QStringLiteral("time"), QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs)},
        {QStringLiteral("pid"), qint64(QCoreApplication::applicationPid())},
        {QStringLiteral("thread"), QString::number(quintptr(QThread::currentThreadId()), 16)},
        {QStringLiteral("session"), session},
        {QStringLiteral("stage"), stage},
        {QStringLiteral("details"), details},
    };
    file.write(QJsonDocument(event).toJson(QJsonDocument::Compact) + '\n');
}

QJsonObject TerminalViewportTrace::describeFrame(const Frame &frame)
{
    int contentCells = 0;
    int firstContentRow = -1;
    int lastContentRow = -1;
    for (int row = 0; row < frame.lines.size(); ++row) {
        for (const Cell &cell : frame.lines.at(row).cells) {
            if (!cell.continuation && !cell.text.trimmed().isEmpty()) {
                ++contentCells;
                if (firstContentRow < 0) firstContentRow = row;
                lastContentRow = row;
            }
        }
    }
    return {{QStringLiteral("columns"), frame.columns},
            {QStringLiteral("rows"), frame.rows},
            {QStringLiteral("contentCells"), contentCells},
            {QStringLiteral("firstContentRow"), firstContentRow},
            {QStringLiteral("lastContentRow"), lastContentRow},
            {QStringLiteral("cursorRow"), frame.cursor.row},
            {QStringLiteral("cursorColumn"), frame.cursor.column}};
}

} // namespace span::terminal
