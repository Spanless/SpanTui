#pragma once

#include "terminal/render/terminal_ime_anchor_resolver.h"

#include <QColor>
#include <QFont>
#include <QInputMethodEvent>
#include <QList>
#include <QTextCharFormat>
#include <QTextLayout>

namespace span::terminal {

struct TerminalImeTextFormat {
    int start = 0;
    int length = 0;
    QTextCharFormat format;
};

struct TerminalImeLayoutMetrics {
    qreal textWidth = 0.0;
    qreal cursorX = 0.0;
};

// GUI-thread local IME state. It never enters the engine and never emits PTY
// bytes; only QInputMethodEvent::commitString is sent by TerminalView.
class TerminalImeCompositionState final {
public:
    bool active() const;
    const QString &preedit() const;
    const QVector<TerminalImeTextFormat> &formats() const;
    int cursorPosition() const;
    bool cursorVisible() const;
    QColor cursorColor() const;
    const TerminalImeAnchor &anchor() const;
    quint64 anchorFrameSequence() const;
    quint64 anchorTransportEpoch() const;

    void update(const QString &preedit,
                const QList<QInputMethodEvent::Attribute> &attributes);
    void clear();
    bool reanchor(const TerminalImeAnchor &candidate,
                  quint64 frameSequence,
                  quint64 transportEpoch,
                  bool liveViewport,
                  int columns,
                  int rows);
    static Qt::InputMethodQueries frameQueryChanges(bool compositionActive,
                                                     bool anchorChanged,
                                                     bool cursorChanged,
                                                     bool metricsChanged);

    QList<QTextLayout::FormatRange> layoutFormats() const;
    TerminalImeLayoutMetrics layoutMetrics(const QFont &font) const;

private:
    void clearAnchor();

    QString m_preedit;
    QVector<TerminalImeTextFormat> m_formats;
    int m_cursorPosition = 0;
    bool m_cursorVisible = true;
    QColor m_cursorColor;
    TerminalImeAnchor m_anchor;
    quint64 m_anchorFrameSequence = 0;
    quint64 m_anchorTransportEpoch = 0;
};

} // namespace span::terminal
