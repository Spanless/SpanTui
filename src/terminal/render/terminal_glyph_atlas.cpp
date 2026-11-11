#include "terminal/render/terminal_glyph_atlas.h"

#include <QMutexLocker>
#include <QSemaphore>
#include <QSet>
#include <QThread>
#include <QThreadPool>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <vector>

namespace span::terminal {

namespace {

using Page = TerminalGlyphAtlas::Page;

constexpr int kSolidBlock = 3;
// Text fields keep Qt Quick's base size whatever the density, some seventy
// texels square: the first mask page holds several hundred glyphs, the
// largest about ten thousand, more than a screen of distinct CJK text.
constexpr int kFirstMask = 2048;
constexpr int kLargestMask = 8192;
constexpr int kFirstColor = 512;
constexpr int kLargestColor = 2048;
// Changes kept for incremental uploads; a texture further behind reloads
// every row used.
constexpr qsizetype kLogLimit = 4096;
// New glyphs per helper thread load() asks for: below that, starting one
// costs more than it saves.
constexpr qsizetype kGlyphsPerHelper = 16;
constexpr int kMostHelpers = 3;

bool nearlyEqual(qreal left, qreal right)
{
    return std::abs(left - right) <= 0.0001;
}

} // namespace

TerminalGlyphAtlas::TerminalGlyphAtlas(int maskSize, int colorSize)
{
    Sheet &mask = sheet(Page::Mask);
    mask.size = maskSize;
    mask.image = QImage(maskSize, maskSize, QImage::Format_Alpha8);
    mask.cleared = kSolidBlock + 1;
    for (int y = 0; y < mask.cleared; ++y) {
        uchar *line = mask.image.scanLine(y);
        std::memset(line, 0, size_t(mask.image.bytesPerLine()));
        if (y < kSolidBlock) std::memset(line, 0xff, kSolidBlock);
    }
    mask.shelfX = kSolidBlock + 1;
    mask.shelfHeight = kSolidBlock + 1;

    Sheet &color = sheet(Page::Color);
    color.size = colorSize;
    color.image = QImage(colorSize, colorSize, QImage::Format_RGBA8888_Premultiplied);
}

std::optional<QPoint> TerminalGlyphAtlas::insert(Page page, const QImage &image)
{
    Sheet &target = sheet(page);
    Q_ASSERT(image.depth() == target.image.depth());
    const int width = image.width();
    const int height = image.height();
    if (width + 1 > target.size || height + 1 > target.size) return std::nullopt;
    if (target.shelfX + width + 1 > target.size) {
        target.shelfY += target.shelfHeight;
        target.shelfX = 0;
        target.shelfHeight = 0;
    }
    if (target.shelfY + height + 1 > target.size) return std::nullopt;
    const QPoint position(target.shelfX, target.shelfY);
    // One transparent texel between neighbours.
    target.shelfX += width + 1;
    target.shelfHeight = std::max(target.shelfHeight, height + 1);

    QMutexLocker lock(&m_mutex);
    const QRect placed(position, QSize(width + 1, height + 1));
    for (; target.cleared <= placed.bottom(); ++target.cleared) {
        std::memset(target.image.scanLine(target.cleared), 0, size_t(target.image.bytesPerLine()));
    }
    const int depth = target.image.depth() / 8;
    for (int y = 0; y < height; ++y) {
        std::memcpy(target.image.scanLine(position.y() + y) + position.x() * depth, image.constScanLine(y),
                    size_t(width) * depth);
    }
    target.log.push_back({++target.revision, placed});
    if (target.log.size() > kLogLimit) target.log.remove(0, kLogLimit / 2);
    return position;
}

QImage TerminalGlyphAtlas::copy(Page page, const QRect &rect) const
{
    // Only the preparation thread writes, and it is the one asking.
    return sheet(page).image.copy(rect);
}

qreal TerminalGlyphAtlas::usage(Page page) const
{
    const Sheet &target = sheet(page);
    return qreal(target.shelfY + target.shelfHeight) / target.size;
}

QVector<TerminalGlyphAtlas::Upload> TerminalGlyphAtlas::takeUploads(Page page, quint64 &revision) const
{
    QMutexLocker lock(&m_mutex);
    const Sheet &source = sheet(page);
    QVector<Upload> uploads;
    if (revision >= source.revision) return uploads;
    if (revision == 0 || source.log.isEmpty() || source.log.constFirst().revision > revision + 1) {
        if (source.cleared > 0) uploads.push_back({QPoint(), source.image.copy(0, 0, source.size, source.cleared)});
    } else {
        for (const Placed &placed : source.log) {
            if (placed.revision > revision) uploads.push_back({placed.rect.topLeft(), source.image.copy(placed.rect)});
        }
    }
    revision = source.revision;
    return uploads;
}

TerminalGlyphCache::TerminalGlyphCache() = default;

TerminalGlyphCache::~TerminalGlyphCache() = default;

void TerminalGlyphCache::setFonts(const TerminalFontSet &fonts)
{
    if (m_rasterizer && m_fonts.baseFont() == fonts.baseFont()
        && nearlyEqual(m_fonts.devicePixelRatio(), fonts.devicePixelRatio())
        && nearlyEqual(m_fonts.cellWidth(), fonts.cellWidth()) && nearlyEqual(m_fonts.cellHeight(), fonts.cellHeight())
        && nearlyEqual(m_fonts.baseline(), fonts.baseline())) {
        return;
    }
    m_fonts = fonts;
    m_rasterizer.emplace(fonts);
    m_atlas = std::make_shared<TerminalGlyphAtlas>(kFirstMask, kFirstColor);
    m_glyphs.clear();
    m_chrome.clear();
    m_fontTable.clear();
    m_lastFont = -1;
}

int TerminalGlyphCache::fontIndex(const QFont &font)
{
    // Cells of one style share one QFont, so the last match almost always hits.
    if (m_lastFont >= 0 && m_fontTable.at(m_lastFont) == font) return m_lastFont;
    for (int index = 0; index < m_fontTable.size(); ++index) {
        if (m_fontTable.at(index) == font) return m_lastFont = index;
    }
    m_fontTable.push_back(font);
    return m_lastFont = int(m_fontTable.size() - 1);
}

TerminalGlyphCache::GlyphKey TerminalGlyphCache::keyFor(const TerminalGlyphCell &cell)
{
    return {cell.text, fontIndex(cell.font), m_rasterizer->deviceWidth(cell)};
}

void TerminalGlyphCache::load(const QVector<const TerminalRowLayout *> &rows)
{
    struct Miss {
        GlyphKey key;
        const TerminalGlyphCell *cell = nullptr;
        TerminalGlyphRaster raster;
    };
    std::vector<Miss> misses;
    QSet<GlyphKey> seen;
    for (const TerminalRowLayout *row : rows) {
        for (const TerminalGlyphCell &cell : row->glyphCells) {
            GlyphKey key = keyFor(cell);
            if (m_glyphs.contains(key) || seen.contains(key)) continue;
            seen.insert(key);
            misses.push_back({std::move(key), &cell, {}});
        }
    }
    // A few are left to glyph().
    const int helpers = int(std::min<qsizetype>(kMostHelpers, qsizetype(misses.size()) / kGlyphsPerHelper - 1));
    if (helpers <= 0) return;
    if (!m_pool) {
        m_pool = std::make_unique<QThreadPool>();
        m_pool->setMaxThreadCount(std::clamp(QThread::idealThreadCount() - 1, 1, kMostHelpers));
    }

    // Each thread takes the next glyph nobody took yet, with a rasterizer of
    // its own; this one joins in with the cache's.
    std::atomic<size_t> next{0};
    const auto rasterize = [&misses, &next](TerminalGlyphRasterizer &rasterizer) {
        for (size_t index; (index = next.fetch_add(1, std::memory_order_relaxed)) < misses.size();) {
            misses[index].raster = rasterizer.text(*misses[index].cell);
        }
    };
    QSemaphore finished;
    int started = 0;
    for (; started < helpers; ++started) {
        const bool running = m_pool->tryStart([this, &rasterize, &finished]() {
            {
                TerminalGlyphRasterizer rasterizer(m_fonts);
                rasterize(rasterizer);
            }
            finished.release();
        });
        if (!running) break;
    }
    rasterize(*m_rasterizer);
    finished.acquire(started);

    for (Miss &miss : misses) {
        Entry entry = place(std::move(miss.raster));
        entry.lastUse = ++m_clock;
        m_glyphs.insert(miss.key, entry);
    }
}

const TerminalGlyph &TerminalGlyphCache::glyph(const TerminalGlyphCell &cell)
{
    const GlyphKey key = keyFor(cell);
    auto found = m_glyphs.find(key);
    if (found == m_glyphs.end()) found = m_glyphs.insert(key, place(m_rasterizer->text(cell)));
    found->lastUse = ++m_clock;
    return found->glyph;
}

const TerminalGlyph &TerminalGlyphCache::chrome(const TerminalCustomGlyph &glyph)
{
    const std::pair<QString, int> key{glyph.text, int(std::lround(glyph.cell.width() * m_fonts.devicePixelRatio()))};
    auto found = m_chrome.find(key);
    if (found == m_chrome.end()) found = m_chrome.insert(key, place(m_rasterizer->chrome(glyph)));
    found->lastUse = ++m_clock;
    return found->glyph;
}

TerminalGlyphCache::Entry TerminalGlyphCache::place(TerminalGlyphRaster raster)
{
    Entry entry{raster.glyph, QRect(), 0};
    if (raster.image.isNull()) return entry;
    const Page page = raster.glyph.color ? Page::Color : Page::Mask;
    std::optional<QPoint> position = m_atlas->insert(page, raster.image);
    if (!position) {
        replaceAtlas(page);
        position = m_atlas->insert(page, raster.image);
    }
    if (!position) {
        // Larger than a whole page: drawn as nothing.
        entry.glyph.texels = QRectF();
        return entry;
    }
    entry.slot = QRect(*position, raster.image.size());
    entry.glyph.texels.translate(*position);
    return entry;
}

void TerminalGlyphCache::replaceAtlas(Page full)
{
    const std::shared_ptr<const TerminalGlyphAtlas> old = m_atlas;
    const auto grown = [&](Page page, int largest) {
        const int size = old->size(page);
        return page == full ? std::min(largest, size * 2) : size;
    };
    m_atlas = std::make_shared<TerminalGlyphAtlas>(grown(Page::Mask, kLargestMask), grown(Page::Color, kLargestColor));

    // The most recently used glyphs move over, texel for texel, until they
    // take half of each new page; the rest is room for the glyphs to come.
    // A glyph left behind (lastUse 0) is rasterized again should it return.
    std::vector<Entry *> placed;
    for (Entry &entry : m_glyphs) {
        if (!entry.slot.isNull()) placed.push_back(&entry);
    }
    for (Entry &entry : m_chrome) {
        if (!entry.slot.isNull()) placed.push_back(&entry);
    }
    std::sort(placed.begin(), placed.end(),
              [](const Entry *left, const Entry *right) { return left->lastUse > right->lastUse; });
    qint64 room[2] = {qint64(m_atlas->size(Page::Mask)) * m_atlas->size(Page::Mask) / 2,
                      qint64(m_atlas->size(Page::Color)) * m_atlas->size(Page::Color) / 2};
    for (Entry *entry : placed) {
        qint64 &left = room[entry->glyph.color ? 1 : 0];
        left -= qint64(entry->slot.width() + 1) * (entry->slot.height() + 1);
        if (left < 0) entry->lastUse = 0;
    }
    // Tallest first, so shelves waste little.
    std::stable_sort(placed.begin(), placed.end(),
                     [](const Entry *left, const Entry *right) { return left->slot.height() > right->slot.height(); });
    for (Entry *entry : placed) {
        if (entry->lastUse == 0) continue;
        const Page page = entry->glyph.color ? Page::Color : Page::Mask;
        const std::optional<QPoint> position = m_atlas->insert(page, old->copy(page, entry->slot));
        if (!position) {
            entry->lastUse = 0;
            continue;
        }
        entry->glyph.texels.translate(*position - entry->slot.topLeft());
        entry->slot.moveTopLeft(*position);
    }
    m_glyphs.removeIf([](QHash<GlyphKey, Entry>::iterator it) { return it->lastUse == 0; });
    m_chrome.removeIf([](QHash<std::pair<QString, int>, Entry>::iterator it) { return it->lastUse == 0; });
}

} // namespace span::terminal
