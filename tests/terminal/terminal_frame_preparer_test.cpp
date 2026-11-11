#include "terminal/render/terminal_frame_preparer.h"

#include <QSignalSpy>
#include <QTest>
#include <QThread>

#include <algorithm>
#include <atomic>

using namespace span::terminal;

namespace {

Line textLine(const QString &text, bool blink = false) {
    Line line;
    for (const QChar character : text) {
        Cell cell;
        cell.text = QString(character);
        cell.width = 1;
        cell.style.blink = blink && !character.isSpace();
        line.cells.push_back(cell);
    }
    return line;
}

std::shared_ptr<const TerminalFrameSnapshot> snapshotOf(const QStringList &rows,
                                                        quint64 sequence,
                                                        int blinkRow = -1) {
    auto snapshot = std::make_shared<TerminalFrameSnapshot>();
    snapshot->sequence = sequence;
    snapshot->frame.columns = rows.isEmpty() ? 0 : int(rows.first().size());
    snapshot->frame.rows = int(rows.size());
    for (int row = 0; row < rows.size(); ++row) {
        snapshot->frame.lines.push_back(textLine(rows.at(row), row == blinkRow));
    }
    // Rows carry no engine identity unless a test gives them one (withIds).
    snapshot->fullRefresh = true;
    return snapshot;
}

// Gives row `n` the engine identity (ids[n], revisions[n]); revisions
// default to 1.
std::shared_ptr<const TerminalFrameSnapshot> withIds(std::shared_ptr<const TerminalFrameSnapshot> snapshot,
                                                     const QList<quint64> &ids, const QList<quint64> &revisions = {},
                                                     quint64 transportEpoch = 1)
{
    auto copy = std::make_shared<TerminalFrameSnapshot>(*snapshot);
    copy->transportEpoch = transportEpoch;
    for (int row = 0; row < ids.size(); ++row) {
        copy->frame.lines[row].id = TerminalRowId{0, ids.at(row)};
        copy->frame.lines[row].revision = row < revisions.size() ? revisions.at(row) : 1;
    }
    return copy;
}

// Rows of `after` still shared with `before`.
QList<int> sharedRows(const TerminalPreparedFrame &before, const TerminalPreparedFrame &after)
{
    QList<int> shared;
    for (int row = 0; row < after.rows.size(); ++row) {
        if (after.rows.at(row) == before.rows.at(row)) shared.append(row);
    }
    return shared;
}

// The first glyph's ink colour on every row.
QList<QColor> firstInk(const TerminalPreparedFrame &frame)
{
    QList<QColor> colors;
    for (const auto &row : frame.rows) {
        const TerminalCellVertex &ink = row->geometry.vertices.at(0);
        colors.append(QColor(ink.r, ink.g, ink.b, ink.a));
    }
    return colors;
}

int verticesSampling(const TerminalPreparedRow &row, const QPointF &texel)
{
    return int(std::count_if(row.geometry.vertices.cbegin(), row.geometry.vertices.cend(),
                             [&](const TerminalCellVertex &vertex) {
                                 return vertex.u == float(texel.x()) && vertex.v == float(texel.y());
                             }));
}

QList<std::shared_ptr<const TerminalFrameSnapshot>> numberedSnapshots(quint64 count)
{
    QList<std::shared_ptr<const TerminalFrameSnapshot>> snapshots;
    for (quint64 sequence = 1; sequence <= count; ++sequence)
        snapshots.append(snapshotOf({QStringLiteral("frame %1").arg(sequence, 2)}, sequence));
    return snapshots;
}

// Rows without backgrounds or chrome draw one quad (four vertices) per
// glyph, left to right.
const TerminalCellVertex &glyphVertex(const TerminalPreparedRow &row, int glyph)
{
    return row.geometry.vertices.at(glyph * 4);
}

TerminalRenderStyle style(quint64 generation, const QColor &foreground = Qt::black) {
    TerminalRenderStyle result;
    result.generation = generation;
    QFont font(QStringLiteral("Consolas"));
    font.setPixelSize(14);
    result.fonts.setFont(font);
    result.foreground = foreground;
    result.background = Qt::white;
    return result;
}

} // namespace

class TerminalFramePreparerTest final : public QObject {
    Q_OBJECT

private slots:
    void sharesUnchangedRowsByPointer();
    void keepsPreparedRowsOfScrolledLines();
    void neverSharesRowsAcrossTransportsOrWithoutIds();
    void rasterizesEachGlyphOnceIntoTheViewAtlas();
    void rebuildsEveryRowForANewStyle();
    void preparesBlinkRowsInBothPhases();
    void preparesCustomGlyphGeometryOffTheRenderThread();
    void preparesOnlyTheNewestSubmittedSnapshot();
    void restylesTheLastSnapshotWithoutNewOutput();
    void resetDiscardsPreparedFrames();
};

void TerminalFramePreparerTest::sharesUnchangedRowsByPointer() {
    const TerminalRenderStyle current = style(1);
    // One atlas for every frame, as on the preparer thread: rows of another
    // atlas are laid out again.
    TerminalGlyphCache glyphs;
    const auto first = TerminalFramePreparer::prepare(
        nullptr, withIds(snapshotOf({QStringLiteral("alpha"), QStringLiteral("bravo")}, 1), {1, 2}), current, &glyphs);
    // Row 2 changed, so the engine gave it a new revision.
    const auto second = TerminalFramePreparer::prepare(
        first.get(), withIds(snapshotOf({QStringLiteral("alpha"), QStringLiteral("brave")}, 2), {1, 2}, {1, 2}),
        current, &glyphs);

    QCOMPARE(second->rows.size(), 2);
    QCOMPARE(second->rows.at(0), first->rows.at(0));
    QVERIFY(second->rows.at(1) != first->rows.at(1));
    QCOMPARE(second->styleGeneration, quint64(1));
    QCOMPARE(second->cellHeight, current.fonts.cellHeight());
}

void TerminalFramePreparerTest::neverSharesRowsAcrossTransportsOrWithoutIds()
{
    const TerminalRenderStyle current = style(1);
    TerminalGlyphCache glyphs;
    const auto snapshot = snapshotOf({QStringLiteral("same"), QStringLiteral("rows")}, 1);
    const auto first = TerminalFramePreparer::prepare(nullptr, withIds(snapshot, {1, 2}), current, &glyphs);

    // A new engine numbers its rows again: equal ids are other rows.
    const auto restarted =
        TerminalFramePreparer::prepare(first.get(), withIds(snapshot, {1, 2}, {}, 2), current, &glyphs);
    QCOMPARE(sharedRows(*first, *restarted), QList<int>());

    // Rows without an identity are always prepared.
    const auto anonymous = TerminalFramePreparer::prepare(first.get(), snapshot, current, &glyphs);
    QCOMPARE(sharedRows(*first, *anonymous), QList<int>());
}

void TerminalFramePreparerTest::keepsPreparedRowsOfScrolledLines()
{
    const TerminalRenderStyle current = style(1);
    TerminalGlyphCache glyphs;
    const auto first = TerminalFramePreparer::prepare(
        nullptr,
        withIds(snapshotOf({QStringLiteral("one"), QStringLiteral("two"), QStringLiteral("six")}, 1), {11, 12, 13}),
        current, &glyphs);
    // Scrolled back by one line: every line moved down a row.
    const auto second = TerminalFramePreparer::prepare(
        first.get(),
        withIds(snapshotOf({QStringLiteral("new"), QStringLiteral("one"), QStringLiteral("two")}, 2), {10, 11, 12}),
        current, &glyphs);
    // Scrolled forward: line 12 moved up a row; line 13 kept its id but its
    // cells changed (a new revision), so it must not reuse its old row.
    const auto third = TerminalFramePreparer::prepare(
        first.get(), withIds(snapshotOf({QStringLiteral("two"), QStringLiteral("ten")}, 3), {12, 13}, {1, 2}), current,
        &glyphs);

    QCOMPARE(second->rows.at(1), first->rows.at(0));
    QCOMPARE(second->rows.at(2), first->rows.at(1));
    QCOMPARE(second->rows.at(0)->glyphCount, 3); // "new"
    QCOMPARE(third->rows.at(0), first->rows.at(1));
    QVERIFY(third->rows.at(1) != first->rows.at(2));
    QCOMPARE(third->rows.at(1)->glyphCount, 3); // "ten"
}

void TerminalFramePreparerTest::rasterizesEachGlyphOnceIntoTheViewAtlas()
{
    const TerminalRenderStyle current = style(1);
    TerminalGlyphCache glyphs;
    const auto first =
        TerminalFramePreparer::prepare(nullptr, snapshotOf({QStringLiteral("aba")}, 1), current, &glyphs);
    const std::shared_ptr<const TerminalGlyphAtlas> atlas = first->rows.at(0)->geometry.atlas;
    QVERIFY(atlas);
    // The mask page starts at revision 1 (its opaque texel); "a" and "b"
    // follow.
    quint64 revision = 1;
    QCOMPARE(atlas->takeUploads(TerminalGlyphAtlas::Page::Mask, revision).size(), 2);

    const TerminalPreparedRow &row = *first->rows.at(0);
    QCOMPARE(row.geometry.vertices.size(), 12);
    QCOMPARE(row.geometry.indices.size(), 18);
    // Both "a" cells sample the same texels at their own grid positions.
    QCOMPARE(glyphVertex(row, 0).u, glyphVertex(row, 2).u);
    QCOMPARE(glyphVertex(row, 0).v, glyphVertex(row, 2).v);
    QVERIFY(glyphVertex(row, 0).u != glyphVertex(row, 1).u);
    QVERIFY(glyphVertex(row, 0).x < glyphVertex(row, 2).x);

    // A later frame, even with no previous frame to compare, rasterizes only
    // what is new.
    const auto second =
        TerminalFramePreparer::prepare(nullptr, snapshotOf({QStringLiteral("zzb")}, 2), current, &glyphs);
    QCOMPARE(second->rows.at(0)->geometry.atlas, atlas);
    QCOMPARE(atlas->takeUploads(TerminalGlyphAtlas::Page::Mask, revision).size(), 1);
    QCOMPARE(glyphVertex(*second->rows.at(0), 2).u, glyphVertex(row, 1).u);

    // Without a cache every call rasterizes into an atlas of its own.
    const auto uncached = TerminalFramePreparer::prepare(nullptr, snapshotOf({QStringLiteral("b")}, 3), current);
    QVERIFY(uncached->rows.at(0)->geometry.atlas != atlas);
}

void TerminalFramePreparerTest::rebuildsEveryRowForANewStyle() {
    TerminalGlyphCache glyphs;
    const auto snapshot = withIds(snapshotOf({QStringLiteral("same"), QStringLiteral("rows")}, 1), {1, 2});
    const auto first = TerminalFramePreparer::prepare(nullptr, snapshot, style(1), &glyphs);
    const auto recolored = TerminalFramePreparer::prepare(first.get(), snapshot, style(2, Qt::darkRed), &glyphs);

    QCOMPARE(recolored->styleGeneration, quint64(2));
    QCOMPARE(sharedRows(*first, *recolored), QList<int>());
    QCOMPARE(firstInk(*recolored), QList<QColor>(2, QColor(Qt::darkRed)));
}

void TerminalFramePreparerTest::preparesBlinkRowsInBothPhases() {
    const auto frame = TerminalFramePreparer::prepare(
        nullptr, snapshotOf({QStringLiteral("calm"), QStringLiteral("wink")}, 1, 1), style(1));

    QVERIFY(frame->hasBlink);
    QVERIFY(!frame->rows.at(0)->hasBlink);
    const TerminalPreparedRow &blinking = *frame->rows.at(1);
    QVERIFY(blinking.hasBlink);
    // Both phases share the indices; hidden cells are fully transparent.
    QCOMPARE(blinking.blinkHiddenVertices.size(), blinking.geometry.vertices.size());
    QCOMPARE(glyphVertex(blinking, 0).a, uchar(255));
    QCOMPARE(blinking.blinkHiddenVertices.at(0).a, uchar(0));
    QCOMPARE(blinking.blinkHiddenVertices.at(0).x, glyphVertex(blinking, 0).x);
}

void TerminalFramePreparerTest::preparesCustomGlyphGeometryOffTheRenderThread() {
    const auto frame = TerminalFramePreparer::prepare(
        nullptr, snapshotOf({QStringLiteral("─█⣿")}, 1), style(1));

    // Box drawing, block and braille cells are rasterized into the atlas like
    // text: one quad each, sampling their own pixels.
    const TerminalPreparedRow &row = *frame->rows.at(0);
    QCOMPARE(row.glyphCount, 3);
    QCOMPARE(row.geometry.vertices.size(), 12);
    const QPointF solid = TerminalGlyphAtlas::solidTexel() / row.geometry.atlas->size(TerminalGlyphAtlas::Page::Mask);
    QCOMPARE(verticesSampling(row, solid), 0);
    // The full block covers exactly its cell.
    const TerminalCellVertex &blockTopLeft = glyphVertex(row, 1);
    const TerminalCellVertex &blockBottomRight = row.geometry.vertices.at(1 * 4 + 3);
    QCOMPARE(blockTopLeft.x, float(frame->cellWidth));
    QCOMPARE(blockTopLeft.y, 0.0f);
    QCOMPARE(blockBottomRight.x, float(frame->cellWidth * 2));
    QCOMPARE(blockBottomRight.y, float(frame->cellHeight));
}

void TerminalFramePreparerTest::preparesOnlyTheNewestSubmittedSnapshot() {
    TerminalFramePreparer preparer;
    preparer.setStyle(style(1));
    std::atomic<bool> emittedOnGuiThread{false};
    connect(
        &preparer, &TerminalFramePreparer::prepared, this,
        [&]() { emittedOnGuiThread = emittedOnGuiThread || QThread::currentThread() == thread(); },
        Qt::DirectConnection);
    QSignalSpy prepared(&preparer, &TerminalFramePreparer::prepared);

    const auto snapshots = numberedSnapshots(20);
    std::for_each(snapshots.cbegin(), snapshots.cend(), [&](const auto &snapshot) { preparer.submit(snapshot); });
    QTRY_VERIFY(preparer.latest() && preparer.latest()->snapshot->sequence == 20);
    QVERIFY(prepared.count() >= 1);
    QVERIFY(!emittedOnGuiThread);
}

void TerminalFramePreparerTest::restylesTheLastSnapshotWithoutNewOutput() {
    TerminalFramePreparer preparer;
    preparer.setStyle(style(1));
    preparer.submit(snapshotOf({QStringLiteral("idle")}, 7));
    QTRY_VERIFY(preparer.latest() && preparer.latest()->styleGeneration == 1);

    preparer.setStyle(style(2, Qt::darkBlue));
    QTRY_VERIFY(preparer.latest()->styleGeneration == 2);
    QCOMPARE(preparer.latest()->snapshot->sequence, quint64(7));
    const TerminalCellVertex &ink = glyphVertex(*preparer.latest()->rows.at(0), 0);
    QCOMPARE(QColor(ink.r, ink.g, ink.b, ink.a), QColor(Qt::darkBlue));
}

void TerminalFramePreparerTest::resetDiscardsPreparedFrames() {
    TerminalFramePreparer preparer;
    preparer.setStyle(style(1));
    preparer.submit(snapshotOf({QStringLiteral("old")}, 3));
    QTRY_VERIFY(preparer.latest());

    preparer.reset();
    QVERIFY(!preparer.latest());
    // Without a baseline a restyle has nothing to prepare.
    preparer.setStyle(style(2));
    QTest::qWait(50);
    QVERIFY(!preparer.latest());
}

QTEST_MAIN(TerminalFramePreparerTest)

#include "terminal_frame_preparer_test.moc"
