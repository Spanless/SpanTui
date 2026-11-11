// TerminalView input: keys, shortcuts, the clipboard, mouse selection and
// links, the wheel, and the input method. Rendering and the frame pipeline
// are in terminal_view.cpp.

#include "terminal/render/terminal_view.h"

#include "terminal/render/terminal_ime_anchor_resolver.h"
#include "terminal/render/terminal_input_routing.h"
#include "terminal/render/terminal_scene_graph.h"

#include "terminal/api/terminal_session.h"
#include "terminal/terminal_constants.h"

#include <QClipboard>
#include <QFocusEvent>
#include <QGuiApplication>
#include <QHoverEvent>
#include <QInputEvent>
#include <QInputMethod>
#include <QInputMethodEvent>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QQuickWindow>
#include <QRegularExpression>
#include <QStyleHints>
#include <QWheelEvent>

#include <algorithm>
#include <cmath>
#include <limits>

namespace span::terminal {

namespace {

// angleDelta units per wheel notch.
constexpr qreal kWheelNotchDelta = 120.0;
// Wheel acceleration: notches closer than kWheelFastMs apart build up to
// kWheelFastMaxVelocity, closer than kWheelSteadyMs to
// kWheelSteadyMaxVelocity; slower notches decay. A pause longer than
// kWheelResetMs or a reversal starts over at 1.
constexpr qint64 kWheelResetMs = 200;
constexpr qint64 kWheelFastMs = 60;
constexpr qreal kWheelFastBoost = 0.6;
constexpr qreal kWheelFastMaxVelocity = 4.5;
constexpr qint64 kWheelSteadyMs = 120;
constexpr qreal kWheelSteadyBoost = 0.25;
constexpr qreal kWheelSteadyMaxVelocity = 3.0;
constexpr qreal kWheelDecay = 0.2;

// The link under `column`: an OSC 8 hyperlink, else a plain http(s) URL in
// the row's text. A wide glyph's continuation cell belongs to its glyph.
QString linkInLine(const Line &line, int column)
{
    if (column < 0 || column >= line.cells.size()) return {};
    int owner = column;
    while (owner > 0 && line.cells.at(owner).continuation) --owner;
    const QString explicitLink = line.cells.at(owner).style.hyperlink;
    if (!explicitLink.isEmpty()) return explicitLink;

    QString text;
    QVector<int> starts(line.cells.size());
    QVector<int> ends(line.cells.size());
    for (int index = 0; index < line.cells.size(); ++index) {
        const Cell &cell = line.cells.at(index);
        if (cell.continuation) {
            starts[index] = index > 0 ? starts.at(index - 1) : int(text.size());
            ends[index] = index > 0 ? ends.at(index - 1) : int(text.size());
            continue;
        }
        starts[index] = int(text.size());
        text += cell.text.isEmpty() ? QStringLiteral(" ") : cell.text;
        ends[index] = int(text.size());
    }
    static const QRegularExpression urlPattern(QStringLiteral(R"(\bhttps?://[^\s<>\"']+)"),
                                               QRegularExpression::CaseInsensitiveOption);
    static const QString trailingPunctuation = QStringLiteral(".,;:!?)]}");
    QRegularExpressionMatchIterator matches = urlPattern.globalMatch(text);
    while (matches.hasNext()) {
        const QRegularExpressionMatch match = matches.next();
        const int start = int(match.capturedStart());
        int end = int(match.capturedEnd());
        while (end > start && trailingPunctuation.contains(text.at(end - 1))) --end;
        if (starts.at(column) < end && ends.at(column) > start) return text.mid(start, end - start);
    }
    return {};
}

} // namespace

void TerminalView::setShortcutBindings(const QVariantMap &bindings)
{
    if (m_shortcutBindings == bindings) {
        return;
    }
    m_shortcutBindings = bindings;
    m_shortcutTable = TerminalInputRouting::shortcutTable(bindings);
    emit shortcutBindingsChanged();
}

bool TerminalView::handleShortcut(QKeyEvent *event)
{
    // Every bound action whose condition holds runs (copy needs a selection,
    // prompt navigation needs OSC 133 marks). When none applies the chord
    // stays an ordinary key, so copy bound to Ctrl+C still interrupts.
    QStringList applicable;
    for (const QString &action : TerminalInputRouting::shortcutActions(event, m_shortcutTable)) {
        const bool promptAction = action == kActionPreviousPrompt || action == kActionNextPrompt;
        if (action == kActionCopy ? hasSelection() : promptAction ? frame().promptMarks : true) {
            applicable.append(action);
        }
    }
    if (applicable.isEmpty()) {
        return false;
    }
    event->accept();
    if (event->isAutoRepeat()) {
        return true;
    }
    for (const QString &action : applicable) {
        if (action == kActionCopy) {
            copySelection();
        } else if (action == kActionPaste) {
            pasteClipboard();
        } else if (action == kActionPreviousPrompt) {
            revealPrompt(-1);
        } else if (action == kActionNextPrompt) {
            revealPrompt(1);
        } else {
            emit shortcutActivated(action);
        }
    }
    return true;
}

void TerminalView::revealPrompt(int direction)
{
    if (ownsRenderConsumer()) m_session->scrollToPrompt(m_renderConsumerToken, direction);
}

void TerminalView::copySelection()
{
    copyText(selectedText());
}

void TerminalView::copyText(const QString &text)
{
    if (!text.isEmpty()) QGuiApplication::clipboard()->setText(text);
}

void TerminalView::pasteClipboard()
{
    if (ownsRenderConsumer() && m_session->running()) {
        m_session->paste(QGuiApplication::clipboard()->text());
        noteCursorHoldRestart();
    }
}

bool TerminalView::event(QEvent *event)
{
    if (event && event->type() == QEvent::ShortcutOverride && hasActiveFocus() && ownsRenderConsumer()
        && m_session->running()) {
        event->accept();
        return true;
    }
    return QQuickItem::event(event);
}

void TerminalView::keyPressEvent(QKeyEvent *event)
{
    if (!ownsRenderConsumer()) {
        event->ignore();
        return;
    }
    // AltGr chords carry Ctrl+Alt but produce layout characters, so they are
    // never shortcuts. Shortcuts also work on an exited session: its history
    // stays readable.
    if (!m_imeComposition.active() && !m_altGrActive && !TerminalInputRouting::identifiesAltGr(event)
        && handleShortcut(event)) {
        return;
    }
    if (!m_session->running()) {
        event->ignore();
        return;
    }

    // While IME is composing, let Qt deliver inputMethodEvent only — do not
    // also forward the underlying latin keys to the PTY.
    if (m_imeComposition.active()) {
        event->accept();
        return;
    }
    if (TerminalInputRouting::identifiesAltGr(event)) {
        m_altGrActive = true;
        if (event->text().isEmpty()) {
            event->accept();
            return;
        }
    }

    const std::optional<TerminalKeyStroke> stroke = TerminalInputRouting::translateKey(
        event, m_altGrActive, event->isAutoRepeat() ? TerminalKeyEvent::Repeat : TerminalKeyEvent::Press);
    const Qt::KeyboardModifiers modifiers = stroke ? stroke->modifiers : Qt::NoModifier;
    // Plain Ctrl+C copies a selection, otherwise it is immediate
    // foreground-process input. It is fixed, not a configurable shortcut.
    if (TerminalInputRouting::isPlainCtrlC(event, modifiers)) {
        if (event->isAutoRepeat()) {
            event->accept();
            return;
        }
        if (hasSelection()) {
            copySelection();
            event->accept();
            return;
        }
        // Send one raw ETX for each deliberate physical press. The foreground
        // application owns any cancel/exit confirmation policy.
        clearSelection();
        m_session->interrupt();
        noteCursorHoldRestart();
        event->accept();
        return;
    }

    if (!stroke) {
        event->ignore();
        return;
    }
    clearSelection();
    m_forwardedKeys.insert(TerminalInputRouting::physicalKey(event));
    m_session->sendKeyStroke(*stroke);
    noteCursorHoldRestart();
    event->accept();
}

void TerminalView::keyReleaseEvent(QKeyEvent *event)
{
    if (!ownsRenderConsumer() || !m_session->running() || event->isAutoRepeat()) {
        event->accept();
        return;
    }
    const bool altGrRelease = TerminalInputRouting::identifiesAltGr(event);
    if (!m_forwardedKeys.remove(TerminalInputRouting::physicalKey(event))) {
        // Whoever took the press owns the release; the child never saw the
        // key go down, so it must not see it come up.
        if (altGrRelease) {
            m_altGrActive = false;
            event->accept();
        } else {
            event->ignore();
        }
        return;
    }
    const std::optional<TerminalKeyStroke> stroke =
        TerminalInputRouting::translateKey(event, m_altGrActive, TerminalKeyEvent::Release);
    if (altGrRelease) m_altGrActive = false;
    if (!stroke) {
        // Nothing was forwarded: let the key release propagate to QML shortcuts.
        event->ignore();
        return;
    }
    m_session->sendKeyStroke(*stroke);
    event->accept();
}

void TerminalView::mousePressEvent(QMouseEvent *event)
{
    finishImeComposition();
    forceActiveFocus(Qt::MouseFocusReason);
    if (!useLocalMouse(event)) {
        forwardMouse(event, true);
        event->accept();
        return;
    }
    if (event->button() == Qt::MiddleButton) {
        pasteClipboard();
        event->accept();
        return;
    }
    if (event->button() == Qt::RightButton) {
        emit contextMenuRequested(event->position().x(), event->position().y(), linkAt(pointAt(event->position())));
        event->accept();
        return;
    }
    if (event->button() != Qt::LeftButton) {
        event->ignore();
        return;
    }
    const QString pressedLink = linkAt(pointAt(event->position()));
    if (!pressedLink.isEmpty()) {
        clearSelection();
        m_pressedLink = pressedLink;
        m_linkPressPosition = event->position();
        event->accept();
        return;
    }
    // The core counts repeated presses into word and line selection.
    sendSelectionEvent(TerminalSelectionEvent::Kind::Press, event);
    m_selecting = true;
    event->accept();
}

void TerminalView::mouseDoubleClickEvent(QMouseEvent *event)
{
    finishImeComposition();
    if (useLocalMouse(event) && event->button() == Qt::LeftButton) {
        // The second press already reached the core as a repeated click.
        event->accept();
        return;
    }
    QQuickItem::mouseDoubleClickEvent(event);
}

void TerminalView::mouseMoveEvent(QMouseEvent *event)
{
    if (!useLocalMouse(event)) {
        forwardMouse(event, false);
        event->accept();
        return;
    }
    if (!m_pressedLink.isEmpty()) {
        const qreal dragDistance = (event->position() - m_linkPressPosition).manhattanLength();
        if (dragDistance < QGuiApplication::styleHints()->startDragDistance()) {
            event->accept();
            return;
        }
        m_pressedLink.clear();
        // Dragging off a link selects from where it was pressed.
        QMouseEvent press(QEvent::MouseButtonPress, m_linkPressPosition, event->scenePosition(),
                          event->globalPosition(), Qt::LeftButton, Qt::LeftButton, event->modifiers());
        sendSelectionEvent(TerminalSelectionEvent::Kind::Press, &press);
        m_selecting = true;
    }
    if (!m_selecting) {
        event->ignore();
        return;
    }
    sendSelectionEvent(TerminalSelectionEvent::Kind::Drag, event);
    event->accept();
}

void TerminalView::mouseReleaseEvent(QMouseEvent *event)
{
    if (!useLocalMouse(event)) {
        forwardMouse(event, false);
        event->accept();
        return;
    }
    if (event->button() == Qt::LeftButton && !m_pressedLink.isEmpty()) {
        const QString pressedLink = m_pressedLink;
        m_pressedLink.clear();
        if (linkAt(pointAt(event->position())) == pressedLink) emit linkActivated(pressedLink);
        event->accept();
        return;
    }
    if (event->button() == Qt::LeftButton && m_selecting) {
        m_selecting = false;
        sendSelectionEvent(TerminalSelectionEvent::Kind::Release, event);
        event->accept();
        return;
    }
    event->ignore();
}

void TerminalView::wheelEvent(QWheelEvent *event)
{
    if (!ownsRenderConsumer()) {
        event->ignore();
        return;
    }
    finishImeComposition();

    const qreal angleRows = qreal(event->angleDelta().y()) / kWheelNotchDelta;
    const qreal pixelRows = qreal(event->pixelDelta().y()) / std::max<qreal>(1.0, m_fontSet.cellHeight());
    if (qFuzzyIsNull(angleRows) && qFuzzyIsNull(pixelRows)) {
        // A tilt wheel or a sideways touchpad swipe reaches a mouse-reporting
        // child as buttons 6/7; the view itself never scrolls sideways.
        const qreal angleColumns = qreal(event->angleDelta().x()) / kWheelNotchDelta;
        const qreal pixelColumns = qreal(event->pixelDelta().x()) / std::max<qreal>(1.0, m_fontSet.cellWidth());
        if ((qFuzzyIsNull(angleColumns) && qFuzzyIsNull(pixelColumns)) || useLocalMouse(event)) {
            event->ignore();
            return;
        }
        m_protocolWheelColumnRemainder += !qFuzzyIsNull(angleColumns) ? angleColumns : pixelColumns;
        const int steps = int(std::trunc(m_protocolWheelColumnRemainder));
        m_protocolWheelColumnRemainder -= steps;
        const TerminalViewportCell point = liveCellAt(event->position());
        // Qt reports a leftward tilt as a positive x delta.
        const TerminalMouseButton button = steps > 0 ? TerminalMouseButton::WheelLeft : TerminalMouseButton::WheelRight;
        for (int index = 0; index < std::abs(steps); ++index) {
            m_session->sendMouseButton(int(button), true, point.row, point.column, int(event->modifiers()));
        }
        event->accept();
        return;
    }

    if (!useLocalMouse(event)) {
        // Terminal mouse reporting is protocol-scaled, not desktop preference
        // scaled: one physical notch produces one WheelUp/WheelDown report.
        m_protocolWheelStepRemainder += !qFuzzyIsNull(angleRows) ? angleRows : pixelRows;
        const int steps = int(std::trunc(m_protocolWheelStepRemainder));
        if (steps == 0) {
            event->accept();
            return;
        }
        m_protocolWheelStepRemainder -= steps;
        const TerminalViewportCell point = liveCellAt(event->position());
        const TerminalMouseButton button = steps > 0 ? TerminalMouseButton::WheelUp : TerminalMouseButton::WheelDown;
        for (int index = 0; index < std::abs(steps); ++index) {
            m_session->sendMouseButton(int(button), true, point.row, point.column, int(event->modifiers()));
        }
        event->accept();
        return;
    }

    const bool precise = qFuzzyIsNull(angleRows);
    qreal rows = pixelRows;
    if (!precise) {
        const int scrollLines = QGuiApplication::styleHints()->wheelScrollLines();
        if (scrollLines == 0) {
            event->ignore();
            return;
        }
        const int rowsPerNotch = scrollLines < 0 || scrollLines > 10000 ? std::max(1, frame().rows - 1) : scrollLines;
        rows = angleRows * rowsPerNotch;
    }

    if (frame().alternateScreen && !event->modifiers().testFlag(Qt::ShiftModifier)) {
        // Alternate scroll mode, as in xterm and Windows Terminal: a pager or
        // editor on the alternate screen without mouse reporting gets cursor
        // keys, since that screen has no history to scroll.
        m_wheelStepRemainder += rows;
        const int keyRows = int(std::trunc(m_wheelStepRemainder));
        m_wheelStepRemainder -= keyRows;
        if (m_session->running()) {
            for (const TerminalKeyStroke &stroke : TerminalInputRouting::alternateScrollKeys(keyRows))
                m_session->sendKeyStroke(stroke);
        }
        event->accept();
        return;
    }

    if (qFuzzyIsNull(rows)) {
        event->accept();
        return;
    }
    // Positive rows scroll toward history. At an end of the range the event
    // is left to the parent.
    if (rows > 0 ? m_scroll.top() <= double(m_scroll.firstLine()) && !m_scroll.animating()
                 : m_scroll.atBottom() && !m_scroll.animating()) {
        event->ignore();
        return;
    }
    if (precise) {
        // A touchpad already delivers smooth pixel deltas; easing them again
        // would lag behind the fingers.
        if (m_scroll.scrollTo(m_scroll.top() - rows)) handleScrollMoved();
        event->accept();
        return;
    }
    const qreal direction = rows > 0 ? 1.0 : -1.0;
    const qint64 elapsedMs =
        m_wheelVelocityClock.isValid() ? m_wheelVelocityClock.restart() : std::numeric_limits<qint64>::max();
    if (elapsedMs > kWheelResetMs || !qFuzzyCompare(direction, m_lastWheelDirection)) {
        m_wheelVelocity = 1.0;
    } else if (elapsedMs < kWheelFastMs) {
        const qreal boost = qreal(kWheelFastMs - elapsedMs) / qreal(kWheelFastMs) * kWheelFastBoost;
        m_wheelVelocity = std::min(kWheelFastMaxVelocity, m_wheelVelocity + boost);
    } else if (elapsedMs < kWheelSteadyMs) {
        m_wheelVelocity = std::min(kWheelSteadyMaxVelocity, m_wheelVelocity + kWheelSteadyBoost);
    } else {
        m_wheelVelocity = std::max(1.0, m_wheelVelocity - kWheelDecay);
    }
    m_lastWheelDirection = direction;
    rows *= m_wheelVelocity;

    if (m_scroll.animateBy(-rows, m_scrollClock.elapsed())) handleScrollMoved();
    // The window advances the animation once per frame from here on. A notch
    // during a running animation asks for a frame too, in case none is
    // scheduled.
    if (m_scroll.animating() && window()) window()->update();
    event->accept();
}

void TerminalView::inputMethodEvent(QInputMethodEvent *event)
{
    if (!ownsRenderConsumer() || !m_session->running()) {
        event->ignore();
        return;
    }

    const QString preedit = event->preeditString();
    const QString commit = event->commitString();
    const bool hasCommit = !commit.isEmpty();
    const bool hasPreedit = !preedit.isEmpty();

    // Commit and preedit are independent: Windows IMEs can partially commit
    // one segment while returning a new composition in the same event.
    if (hasCommit) {
        clearSelection();
        m_session->sendText(commit);
        noteCursorHoldRestart();
    }
    if (hasPreedit) {
        clearSelection();
    }
    m_imeComposition.update(preedit, event->attributes());
    reanchorComposition();
    scheduleFrameUpdate();
    updateCursorAnimation();
    updateInputMethodQueries(Qt::ImCursorRectangle | Qt::ImAnchorRectangle | Qt::ImInputItemClipRectangle);
    event->accept();
}

QVariant TerminalView::inputMethodQuery(Qt::InputMethodQuery query) const
{
    if (query == Qt::ImEnabled) {
        // Qt reads this only on focus changes and explicit updates, and on
        // Windows detaches the IME from the window while it is false (IME
        // hotkeys such as Ctrl+Space then reach the child as keys). Every
        // change of its inputs must be announced with update(ImEnabled).
        return ownsRenderConsumer() && m_session->running();
    }
    if (query == Qt::ImHints) {
        return QVariant(int(Qt::ImhMultiLine | Qt::ImhNoAutoUppercase | Qt::ImhPreferLowercase));
    }
    if (query == Qt::ImInputItemClipRectangle) {
        return boundingRect().intersected(clipRect());
    }
    if (query == Qt::ImCursorRectangle) {
        if (m_imeComposition.active()) {
            return compositionCursorRectangle();
        }
        const std::optional<Cursor> cursor = inputMethodCursor();
        if (!cursor) {
            return QRectF();
        }
        return QRectF(cursor->column * cellWidth(), cursor->row * cellHeight(), cellWidth(), cellHeight());
    }
    if (query == Qt::ImAnchorRectangle) {
        return inputMethodQuery(Qt::ImCursorRectangle);
    }
    // Surrounding line text lets the OS IME place the candidate window and
    // avoid overlapping the already-committed characters after the caret.
    if (query == Qt::ImCursorPosition || query == Qt::ImAnchorPosition || query == Qt::ImSurroundingText
        || query == Qt::ImCurrentSelection || query == Qt::ImAbsolutePosition || query == Qt::ImTextBeforeCursor
        || query == Qt::ImTextAfterCursor) {
        QString lineText;
        int cursorInLine = 0;
        const std::optional<Cursor> cursor = inputMethodCursor();
        const qsizetype cursorLine = cursor ? frame().viewportRow + qsizetype(cursor->row) : -1;
        if (cursor && cursor->row >= 0 && cursorLine < frame().lines.size()) {
            const Line &line = frame().lines.at(cursorLine);
            int column = 0;
            for (const Cell &cell : line.cells) {
                if (cell.width <= 0) {
                    continue;
                }
                if (column < cursor->column) {
                    cursorInLine += cell.text.size();
                }
                lineText += cell.text;
                column += std::max(1, cell.width);
            }
            cursorInLine = std::clamp(cursorInLine, 0, int(lineText.size()));
        }
        if (query == Qt::ImSurroundingText) {
            return lineText;
        }
        if (query == Qt::ImCursorPosition || query == Qt::ImAnchorPosition || query == Qt::ImAbsolutePosition) {
            return cursorInLine;
        }
        if (query == Qt::ImTextBeforeCursor) {
            return lineText.left(cursorInLine);
        }
        if (query == Qt::ImTextAfterCursor) {
            return lineText.mid(cursorInLine);
        }
        if (query == Qt::ImCurrentSelection) {
            return QString();
        }
    }
    return QQuickItem::inputMethodQuery(query);
}

std::optional<Cursor> TerminalView::inputMethodCursor() const
{
    const TerminalImeAnchor anchor = m_imeComposition.active() ? m_imeComposition.anchor() : resolveCursor().imeAnchor;
    if (!anchor.isValid()) {
        return std::nullopt;
    }
    return anchor.cursor;
}

QRectF TerminalView::compositionCursorRectangle() const
{
    if (!m_imeComposition.active() || !m_imeComposition.anchor().isValid()) {
        return {};
    }
    const Cursor &anchor = m_imeComposition.anchor().cursor;
    const qreal viewportWidth = std::max<qreal>(1.0, std::min(width(), frame().columns * cellWidth()));
    const TerminalSceneGraph::PreeditPlacement placement = TerminalSceneGraph::placePreedit(
        m_imeComposition, m_fontSet.baseFont(), anchor.column * cellWidth(), cellWidth(), viewportWidth);
    const qreal caretWidth =
        std::max<qreal>(1.0 / std::max<qreal>(1.0, m_fontSet.devicePixelRatio()), cellWidth() * 0.12);
    return QRectF(placement.cursorX, anchor.row * cellHeight(), caretWidth, cellHeight());
}

bool TerminalView::reanchorComposition()
{
    if (!m_imeComposition.active()) {
        return false;
    }
    return m_imeComposition.reanchor(resolveCursor().imeAnchor, m_lastSnapshotSequence, m_renderTransportEpoch,
                                     m_scroll.atBottom(), frame().columns, frame().rows);
}

void TerminalView::clearImeComposition(bool resetPlatform)
{
    const bool wasActive = m_imeComposition.active();
    m_imeComposition.clear();
    if (resetPlatform && wasActive && !m_resettingInputMethod && QGuiApplication::inputMethod()) {
        m_resettingInputMethod = true;
        QGuiApplication::inputMethod()->reset();
        m_resettingInputMethod = false;
    }
    if (wasActive) {
        updateCursorAnimation();
        scheduleFrameUpdate();
        updateInputMethodQueries();
    }
}

void TerminalView::finishImeComposition()
{
    if (!m_imeComposition.active() || m_resettingInputMethod) {
        return;
    }
    if (QGuiApplication::inputMethod()) {
        QGuiApplication::inputMethod()->commit();
    }
    if (m_imeComposition.active()) {
        clearImeComposition(false);
    }
}

void TerminalView::updateInputMethodQueries(Qt::InputMethodQueries queries) const
{
    if (!queries || !hasActiveFocus() || !QGuiApplication::inputMethod()) {
        return;
    }
    QGuiApplication::inputMethod()->update(queries);
}

TerminalView::GridPoint TerminalView::pointAt(const QPointF &position) const
{
    const double top = m_scroll.top();
    const double first = std::floor(top);
    // Drawn rows start a fraction of a row above the top edge.
    const qreal y = position.y() + (top - first) * cellHeight();
    const int drawnRows = frame().rows + (top > first ? 1 : 0);
    const int row = std::clamp(int(std::floor(y / cellHeight())), 0, std::max(0, drawnRows - 1));
    const int column = std::clamp(int(std::floor(position.x() / cellWidth())), 0, std::max(0, frame().columns - 1));
    return {qint64(first) + row, column};
}

TerminalViewportCell TerminalView::liveCellAt(const QPointF &position) const
{
    const GridPoint point = pointAt(position);
    return {int(std::clamp<qint64>(point.row - m_scroll.liveTop(), 0, std::max(0, frame().rows - 1))), point.column};
}

QString TerminalView::selectedText() const
{
    return m_session && hasSelection() ? m_session->selectedText() : QString();
}

void TerminalView::clearSelection()
{
    // The highlight goes with the core's next frame.
    if (hasSelection() && ownsRenderConsumer()) m_session->clearSelection(m_renderConsumerToken);
}

void TerminalView::sendSelectionEvent(TerminalSelectionEvent::Kind kind, QMouseEvent *event)
{
    if (!ownsRenderConsumer()) return;
    const GridPoint point = pointAt(event->position());
    const qint64 offset = frame().scrollbar.offset;
    TerminalSelectionEvent selection;
    selection.kind = kind;
    selection.row = int(point.row - offset);
    selection.column = point.column;
    selection.offset = offset;
    // Surface pixels of the frame's viewport, which may sit apart from the
    // local scroll position while the core catches up.
    selection.x = event->position().x();
    selection.y = event->position().y() + (m_scroll.top() - double(offset)) * cellHeight();
    selection.timeMs = qint64(event->timestamp());
    selection.repeatIntervalMs = QGuiApplication::styleHints()->mouseDoubleClickInterval();
    selection.rectangle = event->modifiers().testFlag(Qt::AltModifier);
    m_session->selectionEvent(m_renderConsumerToken, selection);
}

bool TerminalView::useLocalMouse(const QInputEvent *event) const
{
    return !ownsRenderConsumer() || TerminalInputRouting::useLocalMouse(m_session->mouseMode(), event);
}

void TerminalView::forwardMouse(QMouseEvent *event, bool pressed)
{
    if (!ownsRenderConsumer()) return;
    const TerminalViewportCell point = liveCellAt(event->position());
    if (event->button() == Qt::NoButton) {
        m_session->sendMouseMove(point.row, point.column, int(event->modifiers()));
        return;
    }
    const TerminalMouseButton button = TerminalInputRouting::mouseButton(event->button());
    if (event->button() != Qt::LeftButton && event->button() != Qt::RightButton
        && event->button() != Qt::MiddleButton) {
        // Extra buttons have no terminal mouse encoding; never report them as
        // a left click.
        return;
    }
    m_session->sendMouseButton(int(button), pressed, point.row, point.column, int(event->modifiers()));
}

QString TerminalView::linkAt(const GridPoint &point) const
{
    const Line *line = lineAt(point.row);
    return line ? linkInLine(*line, point.column) : QString();
}

QString TerminalView::linkAtCell(int row, int column) const
{
    return linkAt({drawnRows().firstLine + row, column});
}

void TerminalView::hoverMoveEvent(QHoverEvent *event)
{
    // The hand only over a link, even while the child takes mouse reports.
    setCursor(linkAt(pointAt(event->position())).isEmpty() ? Qt::IBeamCursor : Qt::PointingHandCursor);
    if (!useLocalMouse(event) && m_session) {
        const TerminalViewportCell point = liveCellAt(event->position());
        m_session->sendMouseMove(point.row, point.column, int(event->modifiers()));
    }
    event->accept();
}

void TerminalView::focusInEvent(QFocusEvent *event)
{
    QQuickItem::focusInEvent(event);
    if (ownsRenderConsumer()) m_session->setTerminalFocused(true);
    updateInputMethodQueries();
}

void TerminalView::focusOutEvent(QFocusEvent *event)
{
    m_altGrActive = false;
    m_forwardedKeys.clear();
    m_pressedLink.clear();
    if (ownsRenderConsumer()) m_session->setTerminalFocused(false);
    clearImeComposition(true);
    QQuickItem::focusOutEvent(event);
}

} // namespace span::terminal
