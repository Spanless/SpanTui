#include "terminal/render/terminal_frame_preparer.h"

#include "terminal/render/terminal_row_layout.h"

#include <QHash>
#include <QMetaObject>
#include <QMutexLocker>
#include <QThread>

#include <algorithm>
#include <cmath>
#include <mutex>
#include <optional>

namespace span::terminal {

namespace {

bool nearlyEqual(qreal left, qreal right) {
    return std::abs(left - right) <= 0.0001;
}

// Rows built together: all are laid out first, so the glyphs they lack are
// rasterized in one go (TerminalGlyphCache::load), then turned into
// vertices. A screen repeats a few hundred glyphs, so once they are in the
// atlas a row costs its layout and one atlas lookup per cell.
class RowBatch {
public:
    RowBatch(const TerminalRenderStyle &style, TerminalGlyphCache &glyphs) : m_style(style), m_glyphs(glyphs) {}

    void add(const Line &line)
    {
        Pending pending{layout(line, true), {}};
        const bool blinks = std::any_of(line.cells.cbegin(), line.cells.cend(),
                                        [](const Cell &cell) { return !cell.continuation && cell.style.blink; });
        if (blinks) pending.hidden = layout(line, false);
        m_pending.push_back(std::move(pending));
    }

    // The rows added since the last call, in order.
    QVector<std::shared_ptr<const TerminalPreparedRow>> build()
    {
        QVector<const TerminalRowLayout *> layouts;
        layouts.reserve(m_pending.size());
        for (const Pending &pending : std::as_const(m_pending)) layouts.push_back(&pending.layout);
        m_glyphs.load(layouts);

        const qreal dpr = m_style.fonts.devicePixelRatio();
        QVector<std::shared_ptr<const TerminalPreparedRow>> rows;
        rows.reserve(m_pending.size());
        for (const Pending &pending : std::as_const(m_pending)) {
            auto row = std::make_shared<TerminalPreparedRow>();
            row->geometry = TerminalRowGeometryBuilder::build(pending.layout, m_glyphs, dpr);
            row->glyphCount = int(pending.layout.glyphCells.size() + pending.layout.customGlyphs.size());
            if (pending.hidden) {
                // Every glyph is in the atlas by now, so both phases sample one.
                const TerminalRowGeometry hidden = TerminalRowGeometryBuilder::build(*pending.hidden, m_glyphs, dpr);
                if (hidden.atlas == row->geometry.atlas && hidden.indices == row->geometry.indices) {
                    row->blinkHiddenVertices = hidden.vertices;
                    row->hasBlink = true;
                }
            }
            rows.push_back(std::move(row));
        }
        m_pending.clear();
        return rows;
    }

private:
    struct Pending {
        TerminalRowLayout layout;
        // The blink phase with blinking cells hidden.
        std::optional<TerminalRowLayout> hidden;
    };

    TerminalRowLayout layout(const Line &line, bool blinkVisible) const
    {
        return TerminalRowLayoutBuilder::build(line, m_style.fonts, m_style.foreground, m_style.background,
                                               m_style.ansiPalette, blinkVisible);
    }

    const TerminalRenderStyle &m_style;
    TerminalGlyphCache &m_glyphs;
    QVector<Pending> m_pending;
};

struct RowKey {
    TerminalRowId id;
    quint64 revision = 0;

    bool operator==(const RowKey &other) const { return id == other.id && revision == other.revision; }
};

size_t qHash(const RowKey &key, size_t seed = 0) noexcept
{
    return qHashMulti(seed, key.id, key.revision);
}

// Shared by every preparer; stopped when the process exits.
struct PreparationThread {
    QThread thread;

    PreparationThread()
    {
        thread.setObjectName(QStringLiteral("TerminalPrepare"));
        thread.start();
    }
    ~PreparationThread()
    {
        thread.quit();
        thread.wait();
    }
};

QThread *preparationThread()
{
    static PreparationThread shared;
    return &shared.thread;
}

} // namespace

// Lives on the preparation thread and is deleted there: the glyph cache keeps
// that thread's fonts. Outlives its preparer, so queued runs check `owner`.
class TerminalFramePreparer::Worker final : public QObject {
public:
    TerminalGlyphCache glyphs;
    // Held for a whole run; the preparer's destructor clears it.
    std::mutex ownerMutex;
    TerminalFramePreparer *owner = nullptr;
};

TerminalFramePreparer::TerminalFramePreparer(QObject *parent) : QObject(parent) {}

TerminalFramePreparer::~TerminalFramePreparer() {
    if (!m_worker) return;
    {
        // Waits only for this preparer's frame in flight.
        std::lock_guard lock(m_worker->ownerMutex);
        m_worker->owner = nullptr;
    }
    m_worker->deleteLater();
}

std::shared_ptr<const TerminalPreparedFrame>
TerminalFramePreparer::prepare(const TerminalPreparedFrame *previous,
                               std::shared_ptr<const TerminalFrameSnapshot> snapshot, const TerminalRenderStyle &style,
                               TerminalGlyphCache *glyphs)
{
    auto frame = std::make_shared<TerminalPreparedFrame>();
    if (!snapshot) return frame;
    TerminalGlyphCache ownGlyphs;
    if (!glyphs) glyphs = &ownGlyphs;
    glyphs->setFonts(style.fonts);
    frame->snapshot = std::move(snapshot);
    frame->styleGeneration = style.generation;
    frame->cellWidth = style.fonts.cellWidth();
    frame->cellHeight = style.fonts.cellHeight();
    frame->baseline = style.fonts.baseline();
    frame->devicePixelRatio = style.fonts.devicePixelRatio();

    const TerminalFrameSnapshot &current = *frame->snapshot;
    const QVector<Line> &lines = current.frame.lines;
    const TerminalFrameSnapshot *before = previous && previous->snapshot ? previous->snapshot.get() : nullptr;
    // Row ids and revisions belong to one engine, which a new transport
    // replaces; prepared rows are row-relative, so a row that only moved
    // keeps its prepared geometry.
    QHash<RowKey, std::shared_ptr<const TerminalPreparedRow>> reusable;
    if (before && previous->styleGeneration == style.generation && nearlyEqual(previous->baseline, frame->baseline)
        && before->transportEpoch == current.transportEpoch && before->frame.columns == current.frame.columns) {
        const qsizetype count = std::min(before->frame.lines.size(), previous->rows.size());
        reusable.reserve(count);
        for (qsizetype index = 0; index < count; ++index) {
            const Line &line = before->frame.lines.at(index);
            const auto &row = previous->rows.at(index);
            // Rows of a replaced atlas still draw correctly, but must not
            // keep that atlas alive; they are rebuilt into the current one.
            if (line.id.isNull() || !row || row->geometry.atlas != glyphs->atlas()) continue;
            reusable.insert({line.id, line.revision}, row);
        }
    }

    frame->rows.resize(lines.size());
    RowBatch batch(style, *glyphs);
    QVector<qsizetype> batchRows;
    for (qsizetype index = 0; index < lines.size(); ++index) {
        const Line &line = lines.at(index);
        const auto found = line.id.isNull() ? reusable.cend() : reusable.constFind({line.id, line.revision});
        if (found != reusable.cend()) {
            frame->rows[index] = *found;
        } else {
            batch.add(line);
            batchRows.push_back(index);
        }
    }
    const QVector<std::shared_ptr<const TerminalPreparedRow>> built = batch.build();
    for (qsizetype index = 0; index < built.size(); ++index) frame->rows[batchRows.at(index)] = built.at(index);
    for (const auto &row : std::as_const(frame->rows)) {
        frame->glyphCount += row->glyphCount;
        frame->hasBlink = frame->hasBlink || row->hasBlink;
    }
    return frame;
}

void TerminalFramePreparer::ensureWorker()
{
    if (m_worker) return;
    m_worker = new Worker;
    m_worker->owner = this;
    m_worker->moveToThread(preparationThread());
}

void TerminalFramePreparer::setStyle(const TerminalRenderStyle &style) {
    // No thread yet means nothing was ever submitted; the style simply waits.
    QMutexLocker lock(&m_mutex);
    if (m_style.generation == style.generation) return;
    m_style = style;
    m_reprepare = true;
    if (m_pending || m_previous) scheduleLocked();
}

void TerminalFramePreparer::submit(std::shared_ptr<const TerminalFrameSnapshot> snapshot) {
    if (!snapshot) return;
    ensureWorker();
    QMutexLocker lock(&m_mutex);
    m_pending = std::move(snapshot);
    scheduleLocked();
}

std::shared_ptr<const TerminalPreparedFrame> TerminalFramePreparer::latest() const {
    QMutexLocker lock(&m_mutex);
    return m_latest;
}

void TerminalFramePreparer::reset() {
    QMutexLocker lock(&m_mutex);
    ++m_resetEpoch;
    m_pending.reset();
    m_latest.reset();
    m_previous.reset();
    m_reprepare = false;
}

void TerminalFramePreparer::scheduleLocked() {
    if (m_scheduled || !m_worker) return;
    m_scheduled = true;
    Worker *worker = m_worker;
    QMetaObject::invokeMethod(
        worker,
        [worker]() {
            std::lock_guard lock(worker->ownerMutex);
            if (worker->owner) worker->owner->run();
        },
        Qt::QueuedConnection);
}

void TerminalFramePreparer::run() {
    for (;;) {
        std::shared_ptr<const TerminalFrameSnapshot> snapshot;
        std::shared_ptr<const TerminalPreparedFrame> previous;
        TerminalRenderStyle style;
        quint64 epoch = 0;
        {
            QMutexLocker lock(&m_mutex);
            snapshot = std::move(m_pending);
            m_pending.reset();
            if (!snapshot && m_reprepare && m_previous) snapshot = m_previous->snapshot;
            m_reprepare = false;
            if (!snapshot) {
                m_scheduled = false;
                return;
            }
            previous = m_previous;
            if (m_workerStyle.generation != m_style.generation) style = m_style;
            epoch = m_resetEpoch;
        }
        if (style.generation != 0) {
            // The GUI keeps using its own copies of these fonts.
            style.fonts.isolate();
            m_workerStyle = std::move(style);
        }
        std::shared_ptr<const TerminalPreparedFrame> result =
            prepare(previous.get(), std::move(snapshot), m_workerStyle, &m_worker->glyphs);
        {
            QMutexLocker lock(&m_mutex);
            if (epoch != m_resetEpoch) continue;
            m_previous = result;
            m_latest = std::move(result);
        }
        emit prepared();
    }
}

} // namespace span::terminal
