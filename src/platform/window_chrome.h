#pragma once

#include <QAbstractNativeEventFilter>
#include <QObject>
#include <QPointer>
#include <QRect>
#include <QRectF>
#include <QVector>

#include <memory>

class QQuickItem;
class QQuickWindow;
class QWindow;

// Owns the native side of the workspace window's title bar, whose content
// QML draws in the top band (Main.qml). Hit-test math lives in
// window_chrome_geometry (pure, unit-tested).
//   Windows: frameless window; WM_NCCALCSIZE removes the system title bar
//   while WS_CAPTION|WS_THICKFRAME keep the DWM shadow, rounded corners,
//   resize borders and native min/max/restore. QML draws the caption buttons.
//   macOS: native window whose content extends under a transparent title bar;
//   the system traffic-light buttons stay over the band's leading edge, and a
//   press on the band's empty area moves the window.
class WindowChrome final : public QObject, public QAbstractNativeEventFilter {
    Q_OBJECT
    Q_PROPERTY(bool maximized READ maximized NOTIFY maximizedChanged FINAL)
    // True where the system draws the caption buttons (macOS); QML then hides
    // its own minimize/maximize/close buttons.
    Q_PROPERTY(bool systemCaptionButtons READ systemCaptionButtons CONSTANT FINAL)
    // Window-space rect (logical pixels) the system caption buttons cover in
    // a windowed (not full-screen) window: the macOS traffic lights. Empty
    // where QML draws its own buttons. The title band centres on it.
    Q_PROPERTY(QRectF captionButtonsRect READ captionButtonsRect NOTIFY captionButtonsRectChanged FINAL)

public:
    explicit WindowChrome(QObject *parent = nullptr);
    ~WindowChrome() override;

    // Must be called after the QML window exists and before it is shown.
    void attach(QQuickWindow *window);
    // Standard-decorated windows can reuse native transition facts without
    // applying the workspace title bar, hit testing, or frame material policy.
    void observeTransitions(QQuickWindow *window);

    // Restored-state geometry (not the maximized rect), for persistence.
    QRect normalGeometry() const;

    // Caption-button UI: true only while the window is currently zoomed.
    bool maximized() const;
    // Persistence: true if zoomed, or minimized-from-maximized (restore-to-max).
    bool effectivelyMaximized() const;

    bool systemCaptionButtons() const;
    QRectF captionButtonsRect() const { return m_captionButtonsRect; }

    // True while the system still runs an interactive move or resize of
    // `window` (it holds the mouse capture / live resize), whether or not
    // the end of the gesture has been reported yet.
    static bool nativeMoveOrResizeHeld(const QWindow *window);

    Q_INVOKABLE void minimize();
    Q_INVOKABLE void toggleMaximize();
    Q_INVOKABLE void requestClose();
    Q_INVOKABLE void setDarkFrame(bool dark);
    Q_INVOKABLE void setTitleBarHeight(int logicalHeight);

    // Items inside the title bar band that must receive normal mouse input
    // (buttons, breadcrumb controls). Anything unregistered acts as a drag
    // area. Hidden items are skipped at hit-test time.
    Q_INVOKABLE void addInteractiveItem(QQuickItem *item);

    bool nativeEventFilter(const QByteArray &eventType, void *message, qintptr *result) override;

signals:
    void maximizedChanged();
    void captionButtonsRectChanged();
    void interactiveResizeStarted();
    void interactiveResizeEnded();
    // 0 restored, 1 minimized, 2 maximized (the Windows WM_SIZE codes, which
    // every platform reports in).
    void nativeWindowStateChanged(int nativeState);
    void devicePixelRatioChanged();

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    void setMaximized(bool value);
    void setCaptionButtonsRect(const QRectF &rect)
    {
        if (m_captionButtonsRect != rect) {
            m_captionButtonsRect = rect;
            emit captionButtonsRectChanged();
        }
    }
    void syncMaximizedFromNative();
    void trackNormalGeometry();

    QPointer<QQuickWindow> m_window;
    QVector<QPointer<QQuickItem>> m_interactiveItems;
    QRect m_normalGeometry;
    QRectF m_captionButtonsRect;
    int m_titleBarHeight = 48;
    bool m_maximized = false;
    bool m_transitionFactsOnly = false;
    // The platform's window handle and native bookkeeping.
    struct Native;
    std::unique_ptr<Native> m_native;
};
