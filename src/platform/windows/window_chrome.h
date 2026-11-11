#pragma once

#include <QAbstractNativeEventFilter>
#include <QObject>
#include <QPointer>
#include <QRect>
#include <QVector>

#include <memory>

class QQuickItem;
class QQuickWindow;

// Owns the native side of the frameless window: removes the system title bar
// via WM_NCCALCSIZE while keeping WS_CAPTION|WS_THICKFRAME so Windows still
// provides DWM shadow, rounded corners, resize borders, and native
// min/max/restore via WM_SYSCOMMAND. Hit-test math lives in
// window_chrome_geometry (pure, unit-tested).
class WindowChrome final : public QObject, public QAbstractNativeEventFilter {
    Q_OBJECT
    Q_PROPERTY(bool maximized READ maximized NOTIFY maximizedChanged FINAL)

public:
    explicit WindowChrome(QObject *parent = nullptr);
    ~WindowChrome() override;

    // Must be called after the QML window exists and before it is shown.
    void attach(QQuickWindow *window);
    // Standard-decorated windows can reuse native transition facts without
    // applying frameless chrome, hit testing, or DWM material policy.
    void observeTransitions(QQuickWindow *window);

    // Restored-state geometry (not the maximized rect), for persistence.
    QRect normalGeometry() const;

    // Caption-button UI: true only while the window is currently zoomed.
    bool maximized() const;
    // Persistence: true if zoomed, or minimized-from-maximized (restore-to-max).
    bool effectivelyMaximized() const;

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
    void interactiveResizeStarted();
    void interactiveResizeEnded();
    void nativeWindowStateChanged(int nativeState);
    void devicePixelRatioChanged();

private:
    void setMaximized(bool value);
    void syncMaximizedFromNative();
    void trackNormalGeometry();
    void refreshAutoHideTaskbarEdges();
    QRect physicalItemRect(const QQuickItem *item) const;

    QPointer<QQuickWindow> m_window;
    void *m_hwnd = nullptr;
    QVector<QPointer<QQuickItem>> m_interactiveItems;
    QRect m_normalGeometry;
    int m_titleBarHeight = 48;
    struct AutoHideTaskbarCache;
    std::shared_ptr<AutoHideTaskbarCache> m_autoHideTaskbarCache;
    bool m_maximized = false;
    unsigned int m_lastNativeSizeType = 0;
    bool m_transitionFactsOnly = false;
};
