#pragma once

#include "terminal/render/terminal_font_set.h"
#include "terminal/render/terminal_glyph_rasterizer.h"
#include "terminal/render/terminal_row_layout.h"

#include <QFont>
#include <QHash>
#include <QImage>
#include <QMutex>
#include <QPoint>
#include <QRectF>
#include <QVector>

#include <memory>
#include <optional>

class QThreadPool;

namespace span::terminal {

// Two fixed-size pages: `Mask` (one byte a texel: text distance fields,
// drawn cell coverage, an opaque block) and `Color` (premultiplied RGBA8,
// emoji). Written on the preparation thread, uploaded on the render thread.
// An atlas never grows or evicts: a full one is replaced, and rows keep
// theirs alive while shown.
class TerminalGlyphAtlas final {
public:
    enum class Page { Mask, Color };

    struct Upload {
        QPoint position;
        QImage image;
    };

    TerminalGlyphAtlas(int maskSize, int colorSize);

    int size(Page page) const { return sheet(page).size; }
    // Texel centre inside the opaque block of the mask page that untextured
    // quads sample, so one shader draws glyphs, backgrounds and lines alike.
    static QPointF solidTexel() { return {1.5, 1.5}; }

    // Preparation thread. Format_Alpha8 on the mask page, premultiplied
    // RGBA8888 on the colour page; nothing when it no longer fits.
    std::optional<QPoint> insert(Page page, const QImage &image);
    QImage copy(Page page, const QRect &rect) const;
    qreal usage(Page page) const;

    // Render thread: placements after `revision`, which advances; a revision
    // older than the log gets every used row. Each upload includes the
    // transparent texels right of and below it, so filtering never reads
    // unwritten texels.
    QVector<Upload> takeUploads(Page page, quint64 &revision) const;

private:
    struct Placed {
        quint64 revision = 0;
        QRect rect;
    };

    struct Sheet {
        int size = 0;
        QImage image;
        quint64 revision = 1;
        QVector<Placed> log;
        // Rows above this are zeroed: the image is cleared as it fills.
        int cleared = 0;
        // Preparation thread only.
        int shelfX = 0;
        int shelfY = 0;
        int shelfHeight = 0;
    };

    const Sheet &sheet(Page page) const { return m_sheets[page == Page::Color ? 1 : 0]; }
    Sheet &sheet(Page page) { return m_sheets[page == Page::Color ? 1 : 0]; }

    mutable QMutex m_mutex;
    Sheet m_sheets[2];
};

// One per preparer; not thread-safe (the atlas it hands out is). The thread
// that uses it must destroy it: its rasterizer holds that thread's fonts.
class TerminalGlyphCache final {
public:
    TerminalGlyphCache();
    ~TerminalGlyphCache();

    void setFonts(const TerminalFontSet &fonts);

    // Rasterizes missing glyphs, on several threads when there are many.
    // May replace the atlas.
    void load(const QVector<const TerminalRowLayout *> &rows);

    // May replace the atlas.
    const TerminalGlyph &glyph(const TerminalGlyphCell &cell);
    const TerminalGlyph &chrome(const TerminalCustomGlyph &glyph);

    // Null before setFonts(). A full page is replaced by one twice as large
    // (up to a limit) holding the most recently used glyphs. A row drawn
    // while the atlas was replaced must be built again.
    std::shared_ptr<const TerminalGlyphAtlas> atlas() const { return m_atlas; }

private:
    struct GlyphKey {
        QString text;
        int font = 0;
        int width = 0;

        bool operator==(const GlyphKey &other) const
        {
            return text == other.text && font == other.font && width == other.width;
        }
        friend size_t qHash(const GlyphKey &key, size_t seed = 0)
        {
            return qHashMulti(seed, key.text, key.font, key.width);
        }
    };

    struct Entry {
        TerminalGlyph glyph;
        QRect slot;
        quint64 lastUse = 0;
    };

    int fontIndex(const QFont &font);
    GlyphKey keyFor(const TerminalGlyphCell &cell);
    Entry place(TerminalGlyphRaster raster);
    void replaceAtlas(TerminalGlyphAtlas::Page full);

    TerminalFontSet m_fonts;
    std::optional<TerminalGlyphRasterizer> m_rasterizer;
    std::unique_ptr<QThreadPool> m_pool;
    std::shared_ptr<TerminalGlyphAtlas> m_atlas;
    QVector<QFont> m_fontTable;
    int m_lastFont = -1;
    quint64 m_clock = 0;
    QHash<GlyphKey, Entry> m_glyphs;
    QHash<std::pair<QString, int>, Entry> m_chrome;
};

} // namespace span::terminal
