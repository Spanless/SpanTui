#pragma once

#include "terminal/api/terminal_types.h"
#include "terminal/render/terminal_font_set.h"
#include "terminal/render/terminal_frame_preparer.h"
#include "terminal/render/terminal_ime_composition_state.h"
#include "terminal/render/terminal_scroll_model.h"
#include "terminal/render/terminal_viewport_lifecycle.h"
#include "terminal/terminal_constants.h"

#include <QColor>
#include <QElapsedTimer>
#include <QFont>
#include <QHash>
#include <QJsonObject>
#include <QMetaObject>
#include <QQuickItem>
#include <QSet>
#include <QTimer>
#include <QVector>

#include <atomic>
#include <memory>
#include <optional>
#include <QVariantList>
#include <QVariantMap>

namespace span::terminal {

class TerminalSession;
struct TerminalCursorResolution;

class TerminalView : public QQuickItem {
    Q_OBJECT
    // Namespace-qualified so the QML type registrar resolves the type.
    Q_PROPERTY(span::terminal::TerminalSession *session READ session WRITE setSession NOTIFY sessionChanged FINAL)
    Q_PROPERTY(QFont font READ font WRITE setFont NOTIFY fontChanged FINAL)
    Q_PROPERTY(QColor foregroundColor READ foregroundColor WRITE setForegroundColor NOTIFY colorsChanged FINAL)
    Q_PROPERTY(QColor backgroundColor READ backgroundColor WRITE setBackgroundColor NOTIFY colorsChanged FINAL)
    Q_PROPERTY(QColor cursorColor READ cursorColor WRITE setCursorColor NOTIFY colorsChanged FINAL)
    Q_PROPERTY(QColor selectionColor READ selectionColor WRITE setSelectionColor NOTIFY colorsChanged FINAL)
    Q_PROPERTY(QColor searchMatchColor READ searchMatchColor WRITE setSearchMatchColor NOTIFY colorsChanged FINAL)
    Q_PROPERTY(QColor searchCurrentColor READ searchCurrentColor WRITE setSearchCurrentColor NOTIFY colorsChanged FINAL)
    Q_PROPERTY(QVariantList ansiPalette READ ansiPalette WRITE setAnsiPalette NOTIFY colorsChanged FINAL)
    Q_PROPERTY(bool unreadOutput READ unreadOutput NOTIFY unreadOutputChanged FINAL)
    Q_PROPERTY(bool hasSelection READ hasSelection NOTIFY selectionChanged FINAL)
    // History rows between the top edge and the active screen; fractional
    // while a row is partly scrolled out.
    Q_PROPERTY(qreal scrollOffset READ scrollOffset NOTIFY scrollChanged FINAL)
    Q_PROPERTY(int scrollbackLines READ scrollbackLines NOTIFY scrollChanged FINAL)
    // Eases wheel scrolling; off under reduced motion.
    Q_PROPERTY(bool smoothScrolling READ smoothScrolling WRITE setSmoothScrolling NOTIFY smoothScrollingChanged FINAL)
    Q_PROPERTY(qreal cellWidth READ cellWidth NOTIFY metricsChanged FINAL)
    Q_PROPERTY(qreal cellHeight READ cellHeight NOTIFY metricsChanged FINAL)
    // Presentation lifecycle is owned by the content container.  It carries
    // no coordinates: moving a tile never changes a terminal viewport.
    Q_PROPERTY(
        int presentationState READ presentationState WRITE setPresentationState NOTIFY presentationStateChanged FINAL)
    Q_PROPERTY(QString cursorStyle READ cursorStyle WRITE setCursorStyle NOTIFY cursorStyleChanged FINAL)
    Q_PROPERTY(
        int maximumFrameRate READ maximumFrameRate WRITE setMaximumFrameRate NOTIFY maximumFrameRateChanged FINAL)
    Q_PROPERTY(bool cursorAnimationEnabled READ cursorAnimationEnabled WRITE setCursorAnimationEnabled NOTIFY
                   cursorAnimationEnabledChanged FINAL)
    // Action id -> portable key sequence. Copy, paste and prompt jumps run
    // here; other ids emit shortcutActivated.
    Q_PROPERTY(QVariantMap shortcutBindings READ shortcutBindings WRITE setShortcutBindings NOTIFY
                   shortcutBindingsChanged FINAL)
    // searchCurrentIndex is 1-based, 0 when no match is selected.
    Q_PROPERTY(bool searchActive READ searchActive NOTIFY searchChanged FINAL)
    Q_PROPERTY(int searchMatchCount READ searchMatchCount NOTIFY searchChanged FINAL)
    Q_PROPERTY(int searchCurrentIndex READ searchCurrentIndex NOTIFY searchChanged FINAL)
    Q_PROPERTY(bool searchPending READ searchPending NOTIFY searchChanged FINAL)

public:
    explicit TerminalView(QQuickItem *parent = nullptr);
    ~TerminalView() override;

    TerminalSession *session() const;
    void setSession(TerminalSession *session);
    QFont font() const;
    void setFont(const QFont &font);
    QColor foregroundColor() const;
    void setForegroundColor(const QColor &color);
    QColor backgroundColor() const;
    void setBackgroundColor(const QColor &color);
    QColor cursorColor() const;
    void setCursorColor(const QColor &color);
    QColor selectionColor() const;
    void setSelectionColor(const QColor &color);
    QColor searchMatchColor() const { return m_searchMatchColor; }
    void setSearchMatchColor(const QColor &color);
    QColor searchCurrentColor() const { return m_searchCurrentColor; }
    void setSearchCurrentColor(const QColor &color);
    bool searchActive() const { return !m_searchNeedle.isEmpty(); }
    int searchMatchCount() const;
    int searchCurrentIndex() const;
    bool searchPending() const;
    QVariantList ansiPalette() const;
    void setAnsiPalette(const QVariantList &palette);
    bool unreadOutput() const;
    bool hasSelection() const;
    qreal scrollOffset() const;
    int scrollbackLines() const;
    bool smoothScrolling() const { return m_smoothScrolling; }
    void setSmoothScrolling(bool enabled);
    qreal cellWidth() const;
    qreal cellHeight() const;
    int presentationState() const;
    void setPresentationState(int state);
    QString cursorStyle() const;
    void setCursorStyle(const QString &style);
    int maximumFrameRate() const;
    void setMaximumFrameRate(int framesPerSecond);
    bool cursorAnimationEnabled() const;
    void setCursorAnimationEnabled(bool enabled);
    QVariantMap shortcutBindings() const { return m_shortcutBindings; }
    void setShortcutBindings(const QVariantMap &bindings);

    Q_INVOKABLE void copySelection();
    Q_INVOKABLE QString selectedText() const;
    // An empty query clears the search; a new query scrolls to its newest match.
    Q_INVOKABLE void setSearch(const QString &query);
    // backwards: the next older match.
    Q_INVOKABLE void findNext(bool backwards);
    Q_INVOKABLE void clearSearch();
    Q_INVOKABLE void copyText(const QString &text);
    Q_INVOKABLE void pasteClipboard();
    Q_INVOKABLE void scrollToBottom();
    // Immediate; ends any scrolling animation.
    Q_INVOKABLE void setScrollOffset(qreal offset);
    // Scrolls to the OSC 133 prompt above (direction < 0) or below.
    void revealPrompt(int direction);
    // Link under a cell of the drawn rows (row 0: the top one).
    QString linkAtCell(int row, int column) const;
    Q_INVOKABLE void commitViewport();

signals:
    void sessionChanged();
    void fontChanged();
    void colorsChanged();
    void unreadOutputChanged();
    void selectionChanged();
    void scrollChanged();
    void contextMenuRequested(qreal x, qreal y, const QString &link);
    void linkActivated(const QString &link);
    void metricsChanged();
    void presentationStateChanged();
    void cursorStyleChanged();
    void maximumFrameRateChanged();
    void cursorAnimationEnabledChanged();
    void smoothScrollingChanged();
    void shortcutBindingsChanged();
    void shortcutActivated(const QString &id);
    void searchChanged();

protected:
    bool event(QEvent *event) override;
    QSGNode *updatePaintNode(QSGNode *oldNode, UpdatePaintNodeData *) override;
    void updatePolish() override;
    void geometryChange(const QRectF &newGeometry, const QRectF &oldGeometry) override;
    void keyPressEvent(QKeyEvent *event) override;
    void keyReleaseEvent(QKeyEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseDoubleClickEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void wheelEvent(QWheelEvent *event) override;
    void inputMethodEvent(QInputMethodEvent *event) override;
    QVariant inputMethodQuery(Qt::InputMethodQuery query) const override;
    void hoverMoveEvent(QHoverEvent *event) override;
    void focusInEvent(QFocusEvent *event) override;
    void focusOutEvent(QFocusEvent *event) override;

private:
    // A cell by absolute row: rows counted from the oldest history row, as
    // the frame scrollbar counts them.
    struct GridPoint {
        qint64 row = 0;
        int column = 0;
    };

    // The rows drawn for the current scroll position, top first.
    struct DrawnRows {
        // Null where the presented frame has no such row.
        QVector<std::shared_ptr<const TerminalPreparedRow>> rows;
        // Absolute row of the first drawn row.
        qint64 firstLine = 0;
        // Index in the frame's lines of the first drawn row.
        qint64 firstIndex = 0;
        // Part of the first row scrolled out above the top edge, in [0, 1).
        qreal fraction = 0;
        // Drawn-row index of the frame viewport's first row.
        int liveRow = 0;
        // Drawn rows the frame does not hold.
        int missing = 0;
    };

    void invalidateAll();
    void scheduleFrameUpdate();
    const Frame &frame() const;
    // By absolute row, overscan included.
    const Line *lineAt(qint64 row) const;
    DrawnRows drawnRows() const;
    DrawnRows rowsFrom(double top) const;
    static QVector<TerminalHighlight> drawnHighlights(const QVector<TerminalHighlight> &highlights,
                                                      const DrawnRows &drawn);
    void syncScroll(const TerminalFrameSnapshot *previous);
    bool adoptFrameViewport();
    void handleScrollMoved();
    void requestCoreScroll();
    void advanceScrollAnimation();
    bool hasBlinkingText() const;
    void publishRenderStyle();
    void requestPreparation();
    void handleFrameReady(quint64 sequence);
    void handleTransportEpochChanged();
    void resetViewportForLiveSession();
    void syncRenderConsumer();
    bool ownsRenderConsumer() const;
    void refreshViewportMetrics();
    TerminalCursorResolution resolveCursor() const;
    std::optional<Cursor> inputMethodCursor() const;
    QRectF compositionCursorRectangle() const;
    bool reanchorComposition();
    void clearImeComposition(bool resetPlatform);
    void finishImeComposition();
    void updateInputMethodQueries(
        Qt::InputMethodQueries queries = Qt::ImQueryInput
            | Qt::ImInputItemClipRectangle | Qt::ImAbsolutePosition) const;
    GridPoint pointAt(const QPointF &position) const;
    // The active-screen cell under `position`, clamped to the screen; mouse
    // reports address the child's grid.
    TerminalViewportCell liveCellAt(const QPointF &position) const;
    void clearSelection();
    void sendSelectionEvent(TerminalSelectionEvent::Kind kind, QMouseEvent *event);
    bool useLocalMouse(const QInputEvent *event) const;
    void forwardMouse(QMouseEvent *event, bool pressed);
    QString linkAt(const GridPoint &point) const;
    void updateCursorAnimation();
    void resetCursorAnimation();
    void noteCursorHoldRestart();
    bool handleShortcut(QKeyEvent *event);
    // Null unless the presented status belongs to the current needle.
    const TerminalSearchStatus *searchStatus() const;
    CursorShape defaultCursorShape() const;
    void sampleContentFrameRate();
    void syncFrameRateSampling();
    void resetContentFrameRate();
    TerminalSession *m_session = nullptr;
    quint64 m_renderConsumerToken = 0;
    QFont m_font;
    QColor m_foreground = QColor(QStringLiteral("#202020"));
    QColor m_background = Qt::transparent;
    QColor m_cursor = QColor(QStringLiteral("#a71930"));
    QColor m_selection = QColor(167, 25, 48, 80);
    QVector<QColor> m_ansiPalette;
    TerminalFontSet m_fontSet;
    TerminalRenderStyle m_renderStyle;
    std::unique_ptr<TerminalFramePreparer> m_preparer;
    std::shared_ptr<const TerminalFrameSnapshot> m_submittedSnapshot;
    std::shared_ptr<const TerminalPreparedFrame> m_presented;
    bool m_selecting = false;
    QTimer m_textBlinkTimer;
    QTimer m_cursorAnimationTimer;
    QTimer m_cursorHoldTimer;
    QTimer m_frameRateTimer;
    QElapsedTimer m_cursorAnimationClock;
    QElapsedTimer m_cursorHoldClock;
    QElapsedTimer m_frameRateClock;
    TerminalScrollModel m_scroll;
    QElapsedTimer m_scrollClock;
    // The last viewport asked of the core: an absolute row, kBottomRow, or
    // kNoRow before the first request.
    static constexpr qint64 kBottomRow = -1;
    static constexpr qint64 kNoRow = -2;
    qint64 m_requestedRow = kNoRow;
    // Id of the last scroll request; frames echo the one they reflect.
    quint64 m_scrollRequest = 0;
    quint64 m_viewportJump = 0;
    bool m_smoothScrolling = true;
    quint64 m_lastSnapshotSequence = 0;
    quint64 m_renderTransportEpoch = 0;
    quint64 m_graphicsRevision = 0;
    quint64 m_contentPresentationSequence = 0;
    QString m_viewportTraceSession;
    int m_viewportTraceFrames = 8;
    int m_viewportTracePaints = 8;
    void traceViewport(const QString &stage, QJsonObject details) const;
    // Alternate scroll mode turns wheel rows into whole cursor-key presses.
    qreal m_wheelStepRemainder = 0;
    qreal m_protocolWheelStepRemainder = 0;
    qreal m_protocolWheelColumnRemainder = 0;
    QElapsedTimer m_wheelVelocityClock;
    qreal m_wheelVelocity = 1.0;
    qreal m_lastWheelDirection = 0.0;
    bool m_textBlinkLit = true;
    bool m_cursorDiscreteLit = true;
    bool m_unreadOutput = false;
    TerminalViewportLifecycle m_viewportLifecycle;
    quint64 m_viewportEpoch = 0;
    // Physical keys whose press reached the session. A release is forwarded
    // only for these: a press the IME consumed (its hotkeys, composition), or
    // one the view handled itself (copy, interrupt), has no release either.
    QSet<quint32> m_forwardedKeys;
    QVariantMap m_shortcutBindings;
    QColor m_searchMatchColor = QColor(255, 196, 0, 90);
    QColor m_searchCurrentColor = QColor(255, 140, 0, 170);
    QString m_searchNeedle;
    QHash<int, QStringList> m_shortcutTable;
    QString m_pressedLink;
    QPointF m_linkPressPosition;
    // Owned by the physical right-Alt lifecycle. Never infer AltGr merely
    // because a printable key happens to carry Ctrl+Alt.
    bool m_altGrActive = false;
    QString m_cursorStyle = kCursorStyleBlock;
    int m_maximumFrameRate = kDefaultFrameRate;
    qreal m_cursorOpacity = 1.0;
    bool m_cursorAnimationEnabled = true;
    // Queued notifications from an earlier binding must not touch the view.
    quint64 m_sessionBindingGeneration = 0;
    // Also changes with a process restart under the same binding, so it
    // must not validate signal connections.
    quint64 m_sceneGeneration = 0;
    bool m_renderConsumerActive = false;
    // Frames that carried new terminal content, counted on the render thread
    // and sampled on the GUI thread; busy content suppresses cursor blinking.
    // The render-thread afterRendering handler shares this counter instead of
    // touching the view: disconnecting does not wait for a call already
    // running on another thread, so that call can outlive the view.
    struct ContentFrameCounter {
        std::atomic<quint64> frames{0};
        std::atomic_bool pending{false};
    };
    int m_terminalContentFps = 0;
    quint64 m_sampledContentFrames = 0;
    const std::shared_ptr<ContentFrameCounter> m_contentFrames =
        std::make_shared<ContentFrameCounter>();
    struct WindowBinding {
        QVector<QMetaObject::Connection> connections;
        quint64 generation = 0;

        void reset() {
            ++generation;
            for (const auto &connection : std::as_const(connections)) {
                QObject::disconnect(connection);
            }
            connections.clear();
        }

        ~WindowBinding() { reset(); }
    };
    WindowBinding m_windowBinding;
    TerminalImeCompositionState m_imeComposition;
    bool m_resettingInputMethod = false;
};

} // namespace span::terminal
