#pragma once

#include "terminal/api/terminal_types.h"
#include "terminal/render/terminal_font_set.h"
#include "terminal/render/terminal_glyph_atlas.h"
#include "terminal/render/terminal_row_geometry.h"

#include <QColor>
#include <QMutex>
#include <QObject>
#include <QVector>

#include <memory>

namespace span::terminal {

// Everything outside the snapshot that decides how a row is laid out. The
// generation changes whenever any field does; a prepared frame is only
// presented under the generation it was built for.
struct TerminalRenderStyle {
    quint64 generation = 0;
    TerminalFontSet fonts;
    QColor foreground;
    QColor background;
    QVector<QColor> ansiPalette;
};

// Immutable once published: what the scene graph uploads for one row, drawn
// with one material over `geometry.atlas`. Rows whose content did not change
// are shared by pointer between consecutive prepared frames.
struct TerminalPreparedRow {
    TerminalRowGeometry geometry;
    // `geometry.vertices` with blinking cells transparent, for the same
    // indices; only set when `hasBlink`.
    QVector<TerminalCellVertex> blinkHiddenVertices;
    // Text, box-drawing, block and braille cells painted.
    int glyphCount = 0;
    bool hasBlink = false;
};

struct TerminalPreparedFrame {
    std::shared_ptr<const TerminalFrameSnapshot> snapshot;
    // One per snapshot frame line, overscan included.
    QVector<std::shared_ptr<const TerminalPreparedRow>> rows;
    quint64 styleGeneration = 0;
    // Metrics the rows were laid out with; zero for a frame without rows.
    qreal cellWidth = 0;
    qreal cellHeight = 0;
    qreal baseline = 0;
    qreal devicePixelRatio = 1;
    int glyphCount = 0;
    bool hasBlink = false;
};

// Prepares on one thread shared by every view: layout, rasterization and
// vertices happen there, so the GUI thread only swaps a pointer. Each
// preparer keeps its own glyph cache. Only the newest submitted snapshot is
// prepared.
class TerminalFramePreparer final : public QObject {
    Q_OBJECT

public:
    explicit TerminalFramePreparer(QObject *parent = nullptr);
    ~TerminalFramePreparer() override;

    // Pure preparation step. `previous` may be null or built for another
    // style; a row is reused wherever the previous frame had the same row id
    // and revision. `glyphs`, when given, must belong to the calling thread;
    // without it the call rasterizes into an atlas of its own.
    static std::shared_ptr<const TerminalPreparedFrame> prepare(const TerminalPreparedFrame *previous,
                                                                std::shared_ptr<const TerminalFrameSnapshot> snapshot,
                                                                const TerminalRenderStyle &style,
                                                                TerminalGlyphCache *glyphs = nullptr);

    // GUI-thread API. All calls are cheap and never wait for preparation.
    void setStyle(const TerminalRenderStyle &style);
    void submit(std::shared_ptr<const TerminalFrameSnapshot> snapshot);
    std::shared_ptr<const TerminalPreparedFrame> latest() const;
    // Drops queued work, the latest result and the reuse baseline.
    void reset();

signals:
    // Emitted on the preparation thread after latest() changed; GUI receivers
    // get it queued.
    void prepared();

private:
    class Worker;
    void ensureWorker();
    void scheduleLocked();
    void run();

    // Deleted on the preparation thread, after this preparer.
    Worker *m_worker = nullptr;
    mutable QMutex m_mutex;
    TerminalRenderStyle m_style;
    std::shared_ptr<const TerminalFrameSnapshot> m_pending;
    std::shared_ptr<const TerminalPreparedFrame> m_latest;
    // Worker-side baseline, guarded by m_mutex for hand-off only.
    std::shared_ptr<const TerminalPreparedFrame> m_previous;
    // Worker thread only: the current style with fonts isolated from the GUI.
    TerminalRenderStyle m_workerStyle;
    quint64 m_resetEpoch = 0;
    // Re-prepare the latest snapshot for a new style.
    bool m_reprepare = false;
    bool m_scheduled = false;
};

} // namespace span::terminal
