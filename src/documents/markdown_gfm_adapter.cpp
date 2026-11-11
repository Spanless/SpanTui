#include "documents/markdown_gfm_adapter.h"

#include <QRegularExpression>
#include <QTextBlock>
#include <QTextDocument>
#include <QVector>

#include <algorithm>

namespace {
constexpr int kMaximumTagLength = 8192;
constexpr int kMaximumHtmlFragmentLength = 1024 * 1024;
constexpr int kMaximumHtmlNesting = 64;
const QString kCenterStartMarker = QStringLiteral("SPANTUI_CENTER_START");
const QString kCenterEndMarker = QStringLiteral("SPANTUI_CENTER_END");

struct Tag {
    int end = -1;
    QString name;
    QString text;
    bool closing = false;
    bool selfClosing = false;
};

QVector<bool> codeMask(const QString &source) {
    QVector<bool> mask(source.size(), false);
    int lineStart = 0;
    QChar fenceCharacter;
    int fenceLength = 0;
    while (lineStart < source.size()) {
        const int newline = source.indexOf(QLatin1Char('\n'), lineStart);
        const int lineEnd = newline < 0 ? source.size() : newline;
        int first = lineStart;
        int spaces = 0;
        while (first < lineEnd && spaces < 4 && source.at(first) == QLatin1Char(' ')) {
            ++first;
            ++spaces;
        }
        int run = 0;
        if (first < lineEnd && (source.at(first) == QLatin1Char('`')
                                || source.at(first) == QLatin1Char('~'))) {
            while (first + run < lineEnd && source.at(first + run) == source.at(first))
                ++run;
        }
        const bool opensFence = fenceLength == 0 && spaces <= 3 && run >= 3
                                && (source.at(first) != QLatin1Char('`')
                                    || !source.mid(first + run, lineEnd - first - run)
                                            .contains(QLatin1Char('`')));
        const bool closesFence = fenceLength > 0 && spaces <= 3
                                 && first < lineEnd && source.at(first) == fenceCharacter
                                 && run >= fenceLength
                                 && source.mid(first + run, lineEnd - first - run).trimmed().isEmpty();
        const bool indented = fenceLength == 0
                              && (spaces >= 4
                                  || (lineStart < lineEnd
                                      && source.at(lineStart) == QLatin1Char('\t')));
        if (fenceLength > 0 || opensFence || indented)
            std::fill(mask.begin() + lineStart,
                      mask.begin() + (newline < 0 ? lineEnd : lineEnd + 1), true);
        if (opensFence) {
            fenceCharacter = source.at(first);
            fenceLength = run;
        } else if (closesFence) {
            fenceCharacter = {};
            fenceLength = 0;
        }
        lineStart = newline < 0 ? source.size() : newline + 1;
    }

    int position = 0;
    while (position < source.size()) {
        if (mask.at(position) || source.at(position) != QLatin1Char('`')) {
            ++position;
            continue;
        }
        const int opening = position;
        while (position < source.size() && source.at(position) == QLatin1Char('`'))
            ++position;
        const int markerLength = position - opening;
        int closing = position;
        while ((closing = source.indexOf(QString(markerLength, QLatin1Char('`')), closing)) >= 0) {
            const bool adjacent = (closing > 0 && source.at(closing - 1) == QLatin1Char('`'))
                                  || (closing + markerLength < source.size()
                                      && source.at(closing + markerLength) == QLatin1Char('`'));
            if (!adjacent) {
                std::fill(mask.begin() + opening,
                          mask.begin() + closing + markerLength, true);
                position = closing + markerLength;
                break;
            }
            closing += markerLength;
        }
    }
    return mask;
}

Tag readTag(const QString &source, int start, const QVector<bool> &mask) {
    Tag tag;
    if (start < 0 || start >= source.size() || mask.at(start)
        || source.at(start) != QLatin1Char('<')) {
        return tag;
    }
    QChar quote;
    const int limit = std::min(int(source.size()), start + kMaximumTagLength);
    int end = start + 1;
    for (; end < limit; ++end) {
        if (mask.at(end))
            return {};
        const QChar character = source.at(end);
        if (!quote.isNull()) {
            if (character == quote)
                quote = {};
        } else if (character == QLatin1Char('\'') || character == QLatin1Char('"')) {
            quote = character;
        } else if (character == QLatin1Char('>')) {
            break;
        }
    }
    if (end >= limit || end >= source.size())
        return {};
    tag.end = end + 1;
    tag.text = source.mid(start, tag.end - start);
    int cursor = 1;
    while (cursor < tag.text.size() && tag.text.at(cursor).isSpace())
        ++cursor;
    if (cursor < tag.text.size() && tag.text.at(cursor) == QLatin1Char('/')) {
        tag.closing = true;
        ++cursor;
    }
    const int nameStart = cursor;
    while (cursor < tag.text.size()
           && (tag.text.at(cursor).isLetterOrNumber()
               || tag.text.at(cursor) == QLatin1Char('-'))) {
        ++cursor;
    }
    tag.name = tag.text.mid(nameStart, cursor - nameStart).toLower();
    tag.selfClosing = tag.text.left(tag.text.size() - 1).trimmed().endsWith(QLatin1Char('/'));
    if (tag.name.isEmpty())
        return {};
    return tag;
}

QString attribute(const QString &tag, const QString &name) {
    const QRegularExpression expression(
        QStringLiteral(R"re((?:^|\s)%1\s*=\s*(?:"([^"]*)"|'([^']*)'|([^\s>]+)))re")
            .arg(QRegularExpression::escape(name)),
        QRegularExpression::CaseInsensitiveOption);
    const QRegularExpressionMatch match = expression.match(tag);
    if (!match.hasMatch())
        return {};
    for (int capture = 1; capture <= 3; ++capture) {
        if (!match.captured(capture).isNull())
            return match.captured(capture);
    }
    return {};
}

int closingTag(const QString &source, const QVector<bool> &mask, int from,
               const QString &name, Tag *closing) {
    int depth = 1;
    const int limit = std::min(int(source.size()), from + kMaximumHtmlFragmentLength);
    int cursor = from;
    while (cursor < limit) {
        cursor = source.indexOf(QLatin1Char('<'), cursor);
        if (cursor < 0 || cursor >= limit)
            break;
        if (source.mid(cursor, 4) == QStringLiteral("<!--")) {
            const int commentEnd = source.indexOf(QStringLiteral("-->"), cursor + 4);
            if (commentEnd < 0 || commentEnd >= limit)
                return -1;
            cursor = commentEnd + 3;
            continue;
        }
        const Tag candidate = readTag(source, cursor, mask);
        if (candidate.end < 0) {
            ++cursor;
            continue;
        }
        if (candidate.name == name) {
            if (candidate.closing)
                --depth;
            else if (!candidate.selfClosing && ++depth > kMaximumHtmlNesting)
                return -1;
            if (depth == 0) {
                *closing = candidate;
                return cursor;
            }
        }
        cursor = candidate.end;
    }
    return -1;
}

QString escapedLabel(QString value) {
    value.replace(QLatin1Char('\\'), QStringLiteral("\\\\"));
    value.replace(QLatin1Char(']'), QStringLiteral("\\]"));
    return value;
}

QString imageMarkdown(const QString &tag, QHash<QString, qreal> *widths) {
    const QString source = attribute(tag, QStringLiteral("src"));
    if (source.isEmpty())
        return {};
    const qreal width = attribute(tag, QStringLiteral("width")).toDouble();
    if (width > 0)
        widths->insert(source, width);
    return QStringLiteral("![%1](%2)")
        .arg(escapedLabel(attribute(tag, QStringLiteral("alt"))), source);
}

void appendBlock(QString *markdown, const QString &replacement) {
    if (!markdown->isEmpty() && !markdown->endsWith(QLatin1Char('\n')))
        markdown->append(QLatin1Char('\n'));
    if (!markdown->isEmpty() && !markdown->endsWith(QStringLiteral("\n\n")))
        markdown->append(QLatin1Char('\n'));
    markdown->append(replacement.trimmed());
    if (!markdown->endsWith(QStringLiteral("\n\n")))
        markdown->append(QStringLiteral("\n\n"));
}

MarkdownGfmAdapter::Result adaptFragment(const QString &source, bool dark) {
    MarkdownGfmAdapter::Result result;
    const QVector<bool> mask = codeMask(source);
    int cursor = 0;
    while (cursor < source.size()) {
        if (mask.at(cursor) || source.at(cursor) != QLatin1Char('<')) {
            result.markdown += source.at(cursor++);
            continue;
        }
        if (source.mid(cursor, 4) == QStringLiteral("<!--")) {
            const int end = source.indexOf(QStringLiteral("-->"), cursor + 4);
            const int length = end < 0 ? source.size() - cursor : end + 3 - cursor;
            result.markdown += source.mid(cursor, length);
            cursor += length;
            continue;
        }
        const Tag tag = readTag(source, cursor, mask);
        if (tag.end < 0 || tag.closing) {
            result.markdown += source.at(cursor++);
            continue;
        }
        if (tag.name == QStringLiteral("pre")) {
            Tag close;
            const int closeStart = closingTag(source, mask, tag.end, tag.name, &close);
            if (closeStart >= 0) {
                result.markdown += source.mid(cursor, close.end - cursor);
                cursor = close.end;
                continue;
            }
        }
        const bool discardedContainer = tag.name == QStringLiteral("script")
                                        || tag.name == QStringLiteral("style")
                                        || tag.name == QStringLiteral("iframe")
                                        || tag.name == QStringLiteral("object")
                                        || tag.name == QStringLiteral("embed");
        if (discardedContainer) {
            Tag close;
            const int closeStart = closingTag(source, mask, tag.end, tag.name, &close);
            if (closeStart >= 0) {
                cursor = close.end;
                continue;
            }
        }
        if (tag.name == QStringLiteral("img")) {
            result.markdown += imageMarkdown(tag.text, &result.imageWidths);
            cursor = tag.end;
            continue;
        }
        if (tag.name == QStringLiteral("br")) {
            result.markdown += QLatin1Char('\n');
            cursor = tag.end;
            continue;
        }
        if (tag.name == QStringLiteral("picture")) {
            Tag close;
            const int closeStart = closingTag(source, mask, tag.end, tag.name, &close);
            if (closeStart >= 0) {
                const QString body = source.mid(tag.end, closeStart - tag.end);
                const QVector<bool> bodyMask(body.size(), false);
                QString selected;
                QString fallback;
                int item = 0;
                while (item < body.size()) {
                    item = body.indexOf(QLatin1Char('<'), item);
                    if (item < 0)
                        break;
                    const Tag child = readTag(body, item, bodyMask);
                    if (child.end < 0) {
                        ++item;
                        continue;
                    }
                    if (child.name == QStringLiteral("source")) {
                        const QString media = attribute(child.text, QStringLiteral("media"));
                        const bool matches = dark
                            ? media.contains(QStringLiteral("dark"), Qt::CaseInsensitive)
                            : media.contains(QStringLiteral("light"), Qt::CaseInsensitive);
                        if (matches)
                            selected = attribute(child.text, QStringLiteral("srcset"))
                                           .section(QLatin1Char(','), 0, 0).trimmed()
                                           .section(QLatin1Char(' '), 0, 0);
                    } else if (child.name == QStringLiteral("img")) {
                        fallback = child.text;
                    }
                    item = child.end;
                }
                if (!selected.isEmpty()) {
                    const qreal width = attribute(fallback, QStringLiteral("width")).toDouble();
                    if (width > 0)
                        result.imageWidths.insert(selected, width);
                    result.markdown += QStringLiteral("![%1](%2)")
                        .arg(escapedLabel(attribute(fallback, QStringLiteral("alt"))), selected);
                } else {
                    result.markdown += imageMarkdown(fallback, &result.imageWidths);
                }
                cursor = close.end;
                continue;
            }
        }
        if (tag.name == QStringLiteral("div") || tag.name == QStringLiteral("h1")
            || tag.name == QStringLiteral("h2") || tag.name == QStringLiteral("h3")
            || tag.name == QStringLiteral("h4") || tag.name == QStringLiteral("h5")
            || tag.name == QStringLiteral("h6") || tag.name == QStringLiteral("a")
            || tag.name == QStringLiteral("code")) {
            Tag close;
            const int closeStart = closingTag(source, mask, tag.end, tag.name, &close);
            if (closeStart >= 0) {
                const MarkdownGfmAdapter::Result inner =
                    adaptFragment(source.mid(tag.end, closeStart - tag.end), dark);
                for (auto iterator = inner.imageWidths.cbegin();
                     iterator != inner.imageWidths.cend(); ++iterator) {
                    result.imageWidths.insert(iterator.key(), iterator.value());
                }
                QString replacement = inner.markdown;
                if (tag.name.startsWith(QLatin1Char('h')) && tag.name.size() == 2) {
                    const QString prefix =
                        QString(tag.name.at(1).digitValue(), QLatin1Char('#'))
                        + QLatin1Char(' ');
                    replacement = replacement.trimmed();
                    const int lastNewline = replacement.lastIndexOf(QLatin1Char('\n'));
                    if (lastNewline >= 0)
                        replacement = replacement.left(lastNewline) + QLatin1Char('\n')
                                      + prefix + replacement.mid(lastNewline + 1).trimmed();
                    else
                        replacement = prefix + replacement;
                } else if (tag.name == QStringLiteral("a")) {
                    const QString href = attribute(tag.text, QStringLiteral("href"));
                    if (!href.isEmpty())
                        replacement = QStringLiteral("[%1](<%2>)").arg(replacement, href);
                } else if (tag.name == QStringLiteral("code")) {
                    int ticks = 1;
                    while (replacement.contains(QString(ticks, QLatin1Char('`'))))
                        ++ticks;
                    const QString delimiter(ticks, QLatin1Char('`'));
                    replacement = delimiter + replacement + delimiter;
                } else if (tag.name == QStringLiteral("div")
                           && attribute(tag.text, QStringLiteral("align"))
                                  .compare(QStringLiteral("center"), Qt::CaseInsensitive) == 0) {
                    replacement = kCenterStartMarker + QStringLiteral("\n\n")
                                  + replacement.trimmed() + QStringLiteral("\n\n")
                                  + kCenterEndMarker;
                }
                const bool blockElement = tag.name == QStringLiteral("div")
                                          || tag.name.startsWith(QLatin1Char('h'));
                if (blockElement)
                    appendBlock(&result.markdown, replacement);
                else
                    result.markdown += replacement;
                cursor = close.end;
                continue;
            }
        }
        result.markdown += tag.text;
        cursor = tag.end;
    }
    return result;
}
}

MarkdownGfmAdapter::Result MarkdownGfmAdapter::adapt(const QString &source, bool dark) {
    MarkdownGfmAdapter::Result result = adaptFragment(source, dark);
    QTextDocument markedDocument;
    markedDocument.setMarkdown(
        result.markdown,
        static_cast<QTextDocument::MarkdownFeatures>(
            QTextDocument::MarkdownDialectGitHub
            | QTextDocument::MarkdownNoHTML));
    int centerDepth = 0;
    int cleanBlockIndex = 0;
    for (QTextBlock block = markedDocument.begin(); block.isValid(); block = block.next()) {
        const QString text = block.text().trimmed();
        if (text == kCenterStartMarker) {
            ++centerDepth;
            continue;
        }
        if (text == kCenterEndMarker) {
            centerDepth = std::max(0, centerDepth - 1);
            continue;
        }
        if (centerDepth > 0)
            result.centeredBlocks.insert(cleanBlockIndex);
        ++cleanBlockIndex;
    }
    result.markdown.replace(kCenterStartMarker + QStringLiteral("\n\n"), QString{});
    result.markdown.replace(QStringLiteral("\n\n") + kCenterEndMarker, QString{});
    result.markdown.replace(kCenterStartMarker, QString{});
    result.markdown.replace(kCenterEndMarker, QString{});
    while (!result.markdown.isEmpty()
           && (result.markdown.back() == QLatin1Char('\n')
               || result.markdown.back() == QLatin1Char('\r'))) {
        result.markdown.chop(1);
    }
    result.markdown.replace(QStringLiteral("> [!IMPORTANT]"),
                            QStringLiteral("> **Important**"),
                            Qt::CaseInsensitive);
    return result;
}
