#include "application/task_progress.h"

#include <QtGlobal>

#include <algorithm>
#include <limits>
#include <utility>

namespace span::application {
namespace {


QString normalizedSource(const QString &source)
{
    return source.trimmed();
}

} // namespace

TaskProgressService::TaskProgressService(QObject *parent)
    : QAbstractListModel(parent)
{
    m_clock.start();
    m_maintenanceTimer.setSingleShot(true);
    m_maintenanceTimer.setTimerType(Qt::PreciseTimer);
    connect(&m_maintenanceTimer, &QTimer::timeout,
            this, &TaskProgressService::maintainEntries);
}

int TaskProgressService::rowCount(const QModelIndex &parent) const
{
    return parent.isValid() ? 0 : m_entries.size();
}

QVariant TaskProgressService::data(const QModelIndex &index, int role) const
{
    if (!index.isValid() || index.row() < 0 || index.row() >= m_entries.size())
        return {};
    const Entry &entry = m_entries.at(index.row());
    switch (role) {
    case SourceRole:
        return entry.source;
    case TitleRole:
        return entry.title;
    case DetailRole:
        return entry.detail;
    case ProgressRole:
        return entry.progress;
    case DeterminateRole:
        return entry.progress >= 0;
    case OverflowRole:
        return entry.progress < 0 || entry.progress >= 1;
    case RunningRole:
        return entry.running;
    case OutcomeRole:
        return int(entry.outcome);
    default:
        return {};
    }
}

QHash<int, QByteArray> TaskProgressService::roleNames() const
{
    return {
        {TitleRole, QByteArrayLiteral("title")},
        {DetailRole, QByteArrayLiteral("detail")},
        {ProgressRole, QByteArrayLiteral("progress")},
        {DeterminateRole, QByteArrayLiteral("determinate")},
        {OverflowRole, QByteArrayLiteral("overflow")},
        {RunningRole, QByteArrayLiteral("running")},
        {SourceRole, QByteArrayLiteral("source")},
        {OutcomeRole, QByteArrayLiteral("outcome")},
    };
}

void TaskProgressService::setPopupOpen(bool open)
{
    if (m_popupOpen == open)
        return;
    m_popupOpen = open;
    emit popupOpenChanged();
}

bool TaskProgressService::start(const QString &source, const QString &title)
{
    const QString key = normalizedSource(source);
    const QString cleanTitle = title.trimmed();
    if (key.isEmpty() || cleanTitle.isEmpty())
        return false;

    const int existing = indexOfSource(key);
    if (existing >= 0) {
        Entry &entry = m_entries[existing];
        entry.title = cleanTitle;
        entry.detail.clear();
        entry.progress = -1;
        entry.running = true;
        entry.outcome = Running;
        entry.startedOrder = ++m_startedOrder;
        entry.revealAtMs = 0;
        entry.finishedAtMs = 0;
        if (existing != 0) {
            beginMoveRows({}, existing, existing, {}, 0);
            m_entries.move(existing, 0);
            endMoveRows();
            emitRow(0);
        } else {
            emitRow(0);
        }
        rebuildAggregates();
        return true;
    }

    Entry &pending = m_pendingEntries[key];
    pending = Entry{};
    pending.source = key;
    pending.title = cleanTitle;
    pending.running = true;
    pending.outcome = Running;
    pending.startedOrder = ++m_startedOrder;
    pending.revealAtMs = m_clock.elapsed() + m_revealDelayMs;
    scheduleMaintenance();
    return true;
}

void TaskProgressService::update(const QString &source, qreal progress, const QString &detail)
{
    const QString key = normalizedSource(source);
    const int row = indexOfSource(key);
    Entry *entryPtr = row >= 0 ? &m_entries[row] : pendingEntry(key);
    if (!entryPtr)
        return;

    Entry &entry = *entryPtr;
    qreal next = progress;
    if (next >= 0)
        next = qBound(0.0, next, 1.0);
    else
        next = -1;
    if (entry.progress >= 0 && next >= 0)
        next = qMax(entry.progress, next);
    else if (entry.progress >= 0 && next < 0)
        next = entry.progress;

    const QString cleanDetail = detail.trimmed();
    if (qFuzzyCompare(entry.progress + 2.0, next + 2.0) && entry.detail == cleanDetail)
        return;

    entry.progress = next;
    entry.detail = cleanDetail;
    if (row >= 0) {
        emitRow(row);
        rebuildAggregates();
    }
}

void TaskProgressService::finish(const QString &source, int outcome, const QString &detail)
{
    const QString key = normalizedSource(source);
    const Outcome resolved = outcome == Failed ? Failed
        : outcome == Cancelled ? Cancelled : Succeeded;
    int row = indexOfSource(key);
    if (row < 0) {
        Entry *pending = pendingEntry(key);
        if (!pending)
            return;
        if (resolved == Succeeded) {
            m_pendingEntries.remove(key);
            scheduleMaintenance();
            return;
        }
        promotePending(key);
        row = indexOfSource(key);
    }
    if (row < 0)
        return;

    Entry &entry = m_entries[row];
    if (!entry.running)
        return;

    if (resolved == Succeeded) {
        beginRemoveRows({}, row, row);
        m_entries.removeAt(row);
        endRemoveRows();
        emit countChanged();
        rebuildAggregates();
        scheduleMaintenance();
        return;
    }

    entry.running = false;
    entry.outcome = resolved;
    const QString cleanDetail = detail.trimmed();
    if (!cleanDetail.isEmpty())
        entry.detail = cleanDetail;
    entry.finishedAtMs = qMax<qint64>(1, m_clock.elapsed());
    emitRow(row);
    resortEntries();
    rebuildAggregates();
    scheduleMaintenance();
}

void TaskProgressService::dismiss(const QString &source)
{
    const int row = indexOfSource(normalizedSource(source));
    if (row < 0)
        return;
    beginRemoveRows({}, row, row);
    m_entries.removeAt(row);
    endRemoveRows();
    emit countChanged();
    rebuildAggregates();
    scheduleMaintenance();
}

int TaskProgressService::indexOfSource(const QString &source) const
{
    if (source.isEmpty())
        return -1;
    for (int i = 0; i < m_entries.size(); ++i) {
        if (m_entries.at(i).source == source)
            return i;
    }
    return -1;
}

TaskProgressService::Entry *TaskProgressService::pendingEntry(const QString &source)
{
    auto it = m_pendingEntries.find(source);
    return it == m_pendingEntries.end() ? nullptr : &it.value();
}

void TaskProgressService::emitRow(int row)
{
    const QModelIndex idx = index(row);
    emit dataChanged(idx, idx);
}

void TaskProgressService::promotePending(const QString &source)
{
    auto it = m_pendingEntries.find(source);
    if (it == m_pendingEntries.end())
        return;
    Entry entry = it.value();
    m_pendingEntries.erase(it);
    entry.revealAtMs = 0;
    beginInsertRows({}, 0, 0);
    m_entries.prepend(std::move(entry));
    endInsertRows();
    emit countChanged();
    resortEntries();
    rebuildAggregates();
}

void TaskProgressService::resortEntries()
{
    if (m_entries.size() < 2)
        return;
    beginResetModel();
    std::stable_sort(m_entries.begin(), m_entries.end(), [](const Entry &a, const Entry &b) {
        if (a.running != b.running)
            return a.running;
        return a.startedOrder > b.startedOrder;
    });
    endResetModel();
}

void TaskProgressService::rebuildAggregates()
{
    const int active = int(std::count_if(m_entries.cbegin(), m_entries.cend(),
                                         [](const Entry &entry) { return entry.running; }));
    // Rows stay sorted running-first, newest-first, so row 0 is the primary.
    const Entry *primary = m_entries.isEmpty() ? nullptr : &m_entries.constFirst();
    const QString title = primary ? primary->title : QString();
    const int outcome = primary ? int(primary->outcome) : Running;

    const bool activeDiffers = active != m_activeCount;
    const bool primaryDiffers = title != m_primaryTitle || outcome != m_primaryOutcome;
    m_activeCount = active;
    m_primaryTitle = title;
    m_primaryOutcome = outcome;
    if (activeDiffers)
        emit activeCountChanged();
    if (primaryDiffers)
        emit primaryChanged();
}

void TaskProgressService::scheduleMaintenance()
{
    const qint64 now = m_clock.elapsed();
    qint64 nextDue = 0;
    for (auto it = m_pendingEntries.cbegin(); it != m_pendingEntries.cend(); ++it) {
        const qint64 remaining = qMax<qint64>(1, it->revealAtMs - now);
        if (nextDue == 0 || remaining < nextDue)
            nextDue = remaining;
    }
    for (const Entry &entry : std::as_const(m_entries)) {
        if (entry.running || entry.finishedAtMs <= 0)
            continue;
        const qint64 remaining = qMax<qint64>(1, entry.finishedAtMs + m_finishedLingerMs - now);
        if (nextDue == 0 || remaining < nextDue)
            nextDue = remaining;
    }
    if (nextDue > 0)
        m_maintenanceTimer.start(int(qMin<qint64>(nextDue, std::numeric_limits<int>::max())));
    else
        m_maintenanceTimer.stop();
}

void TaskProgressService::maintainEntries()
{
    const qint64 now = m_clock.elapsed();
    QStringList ready;
    for (auto it = m_pendingEntries.cbegin(); it != m_pendingEntries.cend(); ++it) {
        if (it->revealAtMs <= now)
            ready.push_back(it.key());
    }
    std::sort(ready.begin(), ready.end(), [this](const QString &a, const QString &b) {
        return m_pendingEntries.value(a).startedOrder > m_pendingEntries.value(b).startedOrder;
    });
    for (const QString &source : std::as_const(ready))
        promotePending(source);

    bool removed = false;
    for (int row = m_entries.size() - 1; row >= 0; --row) {
        const Entry &entry = m_entries.at(row);
        if (entry.running || entry.finishedAtMs <= 0)
            continue;
        const qint64 age = now - entry.finishedAtMs;
        if (age >= m_finishedLingerMs) {
            beginRemoveRows({}, row, row);
            m_entries.removeAt(row);
            endRemoveRows();
            removed = true;
        }
    }
    if (removed)
        emit countChanged();
    rebuildAggregates();
    scheduleMaintenance();
}

} // namespace span::application
