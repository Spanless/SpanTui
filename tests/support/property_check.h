#pragma once

// Seeded property checks for QTest. A check draws its inputs from one
// QRandomGenerator, so a run is repeatable: the failure message names the
// seed and the case, and SPANTUI_PROPERTY_SEED=<seed> replays that run.

#include <QByteArray>
#include <QList>
#include <QRandomGenerator>
#include <QString>
#include <QtGlobal>

#include <algorithm>

namespace span::tests {

inline quint32 propertySeed(quint32 fallback)
{
    bool ok = false;
    const quint32 seed = qEnvironmentVariable("SPANTUI_PROPERTY_SEED").toUInt(&ok);
    return ok ? seed : fallback;
}

// Checks `holds(input)` for `cases` inputs made by `generate(random)`.
// Empty when every case holds; otherwise "seed <s> case <n>: <describe(input)>"
// for the first case that does not.
template <typename Generate, typename Holds, typename Describe>
QString firstCounterexample(quint32 seed, int cases, Generate generate, Holds holds, Describe describe)
{
    const quint32 effective = propertySeed(seed);
    QRandomGenerator random(effective);
    for (int index = 0; index < cases; ++index) {
        const auto input = generate(random);
        if (!holds(input)) return QStringLiteral("seed %1 case %2: %3").arg(effective).arg(index).arg(describe(input));
    }
    return {};
}

inline QString hexOf(const QByteArray &bytes)
{
    return QString::fromLatin1(bytes.toHex(' '));
}

// Up to `maxTokens` tokens from `alphabet`, concatenated.
inline QByteArray tokenString(QRandomGenerator &random, const QList<QByteArray> &alphabet, int maxTokens)
{
    QByteArray bytes;
    const int tokens = random.bounded(maxTokens + 1);
    for (int index = 0; index < tokens; ++index) bytes += alphabet.at(random.bounded(int(alphabet.size())));
    return bytes;
}

// Cut points that split `size` bytes into up to `maxPieces` pieces.
inline QList<qsizetype> randomCuts(QRandomGenerator &random, qsizetype size, int maxPieces)
{
    QList<qsizetype> cuts;
    const int count = size > 1 ? random.bounded(maxPieces) : 0;
    for (int index = 0; index < count; ++index) cuts.append(1 + qsizetype(random.bounded(quint64(size - 1))));
    std::sort(cuts.begin(), cuts.end());
    cuts.erase(std::unique(cuts.begin(), cuts.end()), cuts.end());
    return cuts;
}

// `bytes` split at `cuts` (ascending, inside the array).
inline QList<QByteArray> piecesAt(const QByteArray &bytes, const QList<qsizetype> &cuts)
{
    QList<QByteArray> pieces;
    qsizetype start = 0;
    for (qsizetype cut : cuts) {
        pieces.append(bytes.mid(start, cut - start));
        start = cut;
    }
    pieces.append(bytes.mid(start));
    return pieces;
}

} // namespace span::tests
