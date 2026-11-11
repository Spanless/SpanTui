#include "terminal/render/terminal_ime_composition_state.h"

#include <QTextFormat>
#include <QTextLine>
#include <QTextOption>

#include <algorithm>
#include <limits>

namespace span::terminal {

namespace {

int clampCursorBoundary(const QString &text, int position) {
    int result = std::clamp(position, 0, int(text.size()));
    if (result > 0 && result < text.size()
        && text.at(result - 1).isHighSurrogate()
        && text.at(result).isLowSurrogate()) {
        ++result;
    }
    return result;
}

QTextCharFormat charFormatFrom(const QVariant &value) {
    const QTextFormat textFormat = value.value<QTextFormat>();
    if (textFormat.isCharFormat()) {
        return textFormat.toCharFormat();
    }
    return value.value<QTextCharFormat>();
}

bool cursorInside(const Cursor &cursor, int columns, int rows) {
    return cursor.row >= 0 && cursor.row < rows
        && cursor.column >= 0 && cursor.column < columns;
}

} // namespace

bool TerminalImeCompositionState::active() const { return !m_preedit.isEmpty(); }
const QString &TerminalImeCompositionState::preedit() const { return m_preedit; }
const QVector<TerminalImeTextFormat> &TerminalImeCompositionState::formats() const {
    return m_formats;
}
int TerminalImeCompositionState::cursorPosition() const { return m_cursorPosition; }
bool TerminalImeCompositionState::cursorVisible() const { return m_cursorVisible; }
QColor TerminalImeCompositionState::cursorColor() const { return m_cursorColor; }
const TerminalImeAnchor &TerminalImeCompositionState::anchor() const { return m_anchor; }
quint64 TerminalImeCompositionState::anchorFrameSequence() const {
    return m_anchorFrameSequence;
}
quint64 TerminalImeCompositionState::anchorTransportEpoch() const {
    return m_anchorTransportEpoch;
}

void TerminalImeCompositionState::update(
    const QString &preedit,
    const QList<QInputMethodEvent::Attribute> &attributes) {
    if (preedit.isEmpty()) {
        clear();
        return;
    }

    const bool wasActive = active();
    m_preedit = preedit;
    m_formats.clear();
    m_cursorPosition = preedit.size();
    m_cursorVisible = true;
    m_cursorColor = QColor();

    for (const QInputMethodEvent::Attribute &attribute : attributes) {
        if (attribute.type == QInputMethodEvent::Cursor) {
            m_cursorPosition = clampCursorBoundary(preedit, attribute.start);
            m_cursorVisible = attribute.length != 0;
            const QColor color = attribute.value.value<QColor>();
            if (color.isValid()) {
                m_cursorColor = color;
            }
            continue;
        }
        if (attribute.type != QInputMethodEvent::TextFormat) {
            continue;
        }
        const int start = std::clamp(attribute.start, 0, int(preedit.size()));
        const int end = std::clamp(attribute.start + std::max(0, attribute.length),
                                   start, int(preedit.size()));
        if (end <= start) {
            continue;
        }
        int normalizedStart = start;
        int normalizedEnd = end;
        if (normalizedStart > 0 && normalizedStart < preedit.size()
            && preedit.at(normalizedStart - 1).isHighSurrogate()
            && preedit.at(normalizedStart).isLowSurrogate()) {
            --normalizedStart;
        }
        if (normalizedEnd > 0 && normalizedEnd < preedit.size()
            && preedit.at(normalizedEnd - 1).isHighSurrogate()
            && preedit.at(normalizedEnd).isLowSurrogate()) {
            ++normalizedEnd;
        }
        m_formats.append({normalizedStart, normalizedEnd - normalizedStart,
                          charFormatFrom(attribute.value)});
    }

    if (!wasActive) {
        clearAnchor();
    }
}

void TerminalImeCompositionState::clear() {
    m_preedit.clear();
    m_formats.clear();
    m_cursorPosition = 0;
    m_cursorVisible = true;
    m_cursorColor = QColor();
    clearAnchor();
}

bool TerminalImeCompositionState::reanchor(
    const TerminalImeAnchor &candidate,
    quint64 frameSequence,
    quint64 transportEpoch,
    bool liveViewport,
    int columns,
    int rows) {
    if (!active()) {
        return false;
    }

    const TerminalImeAnchor previous = m_anchor;
    const quint64 previousEpoch = m_anchorTransportEpoch;

    if (!liveViewport || transportEpoch == 0) {
        clearAnchor();
    } else if (m_anchor.isValid()
               && m_anchorTransportEpoch == transportEpoch
               && cursorInside(m_anchor.cursor, columns, rows)) {
        // A terminal child cannot edit or relocate text that is still owned by
        // the platform IME. Keep one insertion anchor for the complete
        // composition instead of following repaint cursors from later frames.
    } else if (candidate.isValid() && cursorInside(candidate.cursor, columns, rows)) {
        m_anchor = candidate;
        m_anchorFrameSequence = frameSequence;
        m_anchorTransportEpoch = transportEpoch;
    } else if (!m_anchor.isValid() || m_anchorTransportEpoch != transportEpoch
               || !cursorInside(m_anchor.cursor, columns, rows)) {
        clearAnchor();
    }

    return previous.source != m_anchor.source
        || previous.cursor.row != m_anchor.cursor.row
        || previous.cursor.column != m_anchor.cursor.column
        || previousEpoch != m_anchorTransportEpoch;
}

Qt::InputMethodQueries TerminalImeCompositionState::frameQueryChanges(
    bool compositionActive,
    bool anchorChanged,
    bool cursorChanged,
    bool metricsChanged) {
    if (metricsChanged) {
        return Qt::ImQueryInput | Qt::ImInputItemClipRectangle
            | Qt::ImAbsolutePosition;
    }
    if (compositionActive) {
        // Background TUI animation does not change an active composition.
        // Notifying ImQueryInput for every terminal frame makes Windows IMEs
        // repeatedly query and reposition their candidate UI.
        return anchorChanged
            ? Qt::InputMethodQueries(Qt::ImCursorRectangle
                                     | Qt::ImAnchorRectangle)
            : Qt::InputMethodQueries{};
    }
    return cursorChanged ? Qt::InputMethodQueries(Qt::ImQueryInput)
                         : Qt::InputMethodQueries{};
}

QList<QTextLayout::FormatRange> TerminalImeCompositionState::layoutFormats() const {
    QList<QTextLayout::FormatRange> result;
    result.reserve(m_formats.size());
    for (const TerminalImeTextFormat &source : m_formats) {
        QTextLayout::FormatRange range;
        range.start = source.start;
        range.length = source.length;
        range.format = source.format;
        result.append(range);
    }
    return result;
}

TerminalImeLayoutMetrics TerminalImeCompositionState::layoutMetrics(
    const QFont &font) const {
    TerminalImeLayoutMetrics result;
    if (!active()) {
        return result;
    }
    QTextLayout layout(m_preedit, font);
    QTextOption option;
    option.setWrapMode(QTextOption::NoWrap);
    layout.setTextOption(option);
    layout.setFormats(layoutFormats());
    layout.beginLayout();
    QTextLine line = layout.createLine();
    if (line.isValid()) {
        line.setLineWidth(std::numeric_limits<qreal>::max());
        line.setPosition(QPointF());
        result.textWidth = line.naturalTextWidth();
        result.cursorX = line.cursorToX(clampCursorBoundary(m_preedit,
                                                            m_cursorPosition));
    }
    layout.endLayout();
    return result;
}

void TerminalImeCompositionState::clearAnchor() {
    m_anchor = {};
    m_anchorFrameSequence = 0;
    m_anchorTransportEpoch = 0;
}

} // namespace span::terminal
