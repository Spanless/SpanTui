#include "platform/window_chrome.h"

#include "platform/window_chrome_geometry.h"

#include <QGuiApplication>
#include <QMouseEvent>
#include <QQuickItem>
#include <QQuickWindow>

#import <AppKit/AppKit.h>

namespace {

// Only the cocoa platform backs a QWindow with an NSView; offscreen test
// runs have no native window at all.
NSWindow *nsWindowOf(const QWindow *window)
{
    if (!window || QGuiApplication::platformName() != QLatin1String("cocoa")) return nil;
    NSView *view = (__bridge NSView *)reinterpret_cast<void *>(window->winId());
    return view.window;
}

// The WM_SIZE codes WindowChrome reports in on every platform.
int nativeStateOf(Qt::WindowStates states)
{
    if (states & Qt::WindowMinimized) return 1;
    if (states & (Qt::WindowMaximized | Qt::WindowFullScreen)) return 2;
    return 0;
}

// What a double click on a title bar does, as chosen in System Settings
// (Desktop & Dock > "Double-click a window's title bar to").
enum class TitleBarDoubleClick { Zoom, Minimize, None };

TitleBarDoubleClick titleBarDoubleClickAction()
{
    NSString *action = [[NSUserDefaults standardUserDefaults] stringForKey:@"AppleActionOnDoubleClick"];
    if ([action isEqualToString:@"Minimize"]) return TitleBarDoubleClick::Minimize;
    if ([action isEqualToString:@"None"]) return TitleBarDoubleClick::None;
    return TitleBarDoubleClick::Zoom;
}

// The rect the traffic lights cover, in the window's top-left logical
// coordinates (the content view spans the whole frame under the transparent
// title bar). Null in full screen, where the buttons live in the separate
// title bar window that slides down over the content.
QRectF captionButtonsRectOf(NSWindow *window)
{
    if (!window || (window.styleMask & NSWindowStyleMaskFullScreen)) return {};
    [window layoutIfNeeded];
    NSRect united = NSZeroRect;
    for (NSWindowButton kind : {NSWindowCloseButton, NSWindowMiniaturizeButton, NSWindowZoomButton}) {
        NSButton *button = [window standardWindowButton:kind];
        if (!button || button.hidden) continue;
        const NSRect frame = [button convertRect:button.bounds toView:nil];
        united = NSIsEmptyRect(united) ? frame : NSUnionRect(united, frame);
    }
    if (NSIsEmptyRect(united)) return {};
    const CGFloat height = window.frame.size.height;
    return QRectF(united.origin.x, height - NSMaxY(united), united.size.width, united.size.height);
}

} // namespace

struct WindowChrome::Native {
    __weak NSWindow *window = nil;
    NSMutableArray<id> *observers = [NSMutableArray array];
    // Last state reported through nativeWindowStateChanged.
    int lastNativeState = 0;

    void removeObservers()
    {
        for (id observer in observers) {
            [[NSNotificationCenter defaultCenter] removeObserver:observer];
        }
        [observers removeAllObjects];
    }

    ~Native() { removeObservers(); }
};

WindowChrome::WindowChrome(QObject *parent) : QObject(parent), m_native(std::make_unique<Native>()) {}

WindowChrome::~WindowChrome() = default;

void WindowChrome::attach(QQuickWindow *window)
{
    Q_ASSERT(window);
    // Native window, content under a transparent title bar: QML draws the
    // band, the system keeps its traffic lights, shadow, resizing, zoom and
    // full screen. The native window is created before the content expands
    // over the title bar, so QML's declared rect stays the frame; created
    // expanded, it would grow upward by the title bar, above the screen.
    const QRect declared = window->geometry();
    window->create();
    window->setFlags((window->flags() & ~Qt::FramelessWindowHint) | Qt::ExpandedClientAreaHint
                     | Qt::NoTitleBarBackgroundHint);
    window->setGeometry(declared);
    // The band carries no title, as on Windows.
    NSWindow *nsWindow = nsWindowOf(window);
    if (nsWindow) nsWindow.titleVisibility = NSWindowTitleHidden;
    observeTransitions(window);
    // The traffic lights keep their place in a windowed window; re-read on
    // resize and on leaving full screen in case AppKit lays the title bar
    // out again. Full screen keeps the last windowed rect.
    QPointer<WindowChrome> self(this);
    const auto syncCaptionButtons = [self] {
        if (!self) return;
        const QRectF rect = captionButtonsRectOf(self->m_native->window);
        if (!rect.isNull()) self->setCaptionButtonsRect(rect);
    };
    syncCaptionButtons();
    if (nsWindow) {
        for (NSNotificationName name : {NSWindowDidResizeNotification, NSWindowDidExitFullScreenNotification}) {
            [m_native->observers addObject:[[NSNotificationCenter defaultCenter] addObserverForName:name
                                                                                             object:nsWindow
                                                                                              queue:nil
                                                                                         usingBlock:^(NSNotification *) {
                                                                                           syncCaptionButtons();
                                                                                         }]];
        }
    }
    m_transitionFactsOnly = false;
    window->installEventFilter(this);

    m_normalGeometry = window->geometry();
    connect(window, &QWindow::xChanged, this, &WindowChrome::trackNormalGeometry);
    connect(window, &QWindow::yChanged, this, &WindowChrome::trackNormalGeometry);
    connect(window, &QWindow::widthChanged, this, &WindowChrome::trackNormalGeometry);
    connect(window, &QWindow::heightChanged, this, &WindowChrome::trackNormalGeometry);
    syncMaximizedFromNative();
}

void WindowChrome::observeTransitions(QQuickWindow *window)
{
    Q_ASSERT(window);
    m_window = window;
    m_transitionFactsOnly = true;
    m_native->removeObservers();
    NSWindow *nsWindow = nsWindowOf(window);
    m_native->window = nsWindow;
    m_native->lastNativeState = nativeStateOf(window->windowStates());

    // A nil object would observe every window in the process.
    if (nsWindow) {
        QPointer<WindowChrome> self(this);
        const auto observe = [&](NSNotificationName name, void (^block)(NSNotification *)) {
            [m_native->observers addObject:[[NSNotificationCenter defaultCenter] addObserverForName:name
                                                                                             object:nsWindow
                                                                                              queue:nil
                                                                                         usingBlock:block]];
        };
        observe(NSWindowWillStartLiveResizeNotification, ^(NSNotification *) {
            if (self) emit self->interactiveResizeStarted();
        });
        observe(NSWindowDidEndLiveResizeNotification, ^(NSNotification *) {
            if (self) emit self->interactiveResizeEnded();
        });
        observe(NSWindowDidChangeBackingPropertiesNotification, ^(NSNotification *) {
            if (self) emit self->devicePixelRatioChanged();
        });
    }

    connect(window, &QWindow::windowStateChanged, this, [this](Qt::WindowState) {
        if (!m_window) return;
        const int state = nativeStateOf(m_window->windowStates());
        if (!m_transitionFactsOnly) syncMaximizedFromNative();
        if (state != m_native->lastNativeState) {
            m_native->lastNativeState = state;
            emit nativeWindowStateChanged(state);
        }
    });
}

QRect WindowChrome::normalGeometry() const
{
    if (m_window && nativeStateOf(m_window->windowStates()) == 0) {
        return m_window->geometry();
    }
    return m_normalGeometry;
}

bool WindowChrome::maximized() const
{
    return m_maximized;
}

bool WindowChrome::effectivelyMaximized() const
{
    // syncMaximizedFromNative keeps the zoom state across a minimize.
    return m_maximized;
}

bool WindowChrome::systemCaptionButtons() const
{
    return true;
}

bool WindowChrome::nativeMoveOrResizeHeld(const QWindow *window)
{
    // Live resize has a reliable end notification on macOS; this only
    // answers whether one is still running.
    NSWindow *nsWindow = nsWindowOf(window);
    return nsWindow && nsWindow.inLiveResize;
}

void WindowChrome::minimize()
{
    if (m_window) m_window->showMinimized();
}

void WindowChrome::toggleMaximize()
{
    if (!m_window) return;
    if (m_window->windowStates() & Qt::WindowFullScreen) {
        m_window->showNormal();
        return;
    }
    if (NSWindow *window = m_native->window) {
        [window zoom:nil];
    }
}

void WindowChrome::requestClose()
{
    if (m_window) m_window->close();
}

void WindowChrome::setDarkFrame(bool dark)
{
    if (NSWindow *window = m_native->window) {
        window.appearance = [NSAppearance appearanceNamed:dark ? NSAppearanceNameDarkAqua : NSAppearanceNameAqua];
    }
}

void WindowChrome::setTitleBarHeight(int logicalHeight)
{
    m_titleBarHeight = logicalHeight;
}

void WindowChrome::addInteractiveItem(QQuickItem *item)
{
    if (item && !m_interactiveItems.contains(item)) {
        m_interactiveItems.push_back(item);
    }
}

bool WindowChrome::nativeEventFilter(const QByteArray &, void *, qintptr *)
{
    return false;
}

// A press on the title band's empty area moves the window, a double click
// acts like one on a native title bar; registered items get their input.
bool WindowChrome::eventFilter(QObject *watched, QEvent *event)
{
    if (watched != m_window || m_transitionFactsOnly) {
        return QObject::eventFilter(watched, event);
    }
    const QEvent::Type type = event->type();
    if (type != QEvent::MouseButtonPress && type != QEvent::MouseButtonDblClick) {
        return QObject::eventFilter(watched, event);
    }
    auto *mouse = static_cast<QMouseEvent *>(event);
    if (mouse->button() != Qt::LeftButton) {
        return QObject::eventFilter(watched, event);
    }
    QVector<QRect> interactive;
    interactive.reserve(m_interactiveItems.size());
    for (const auto &item : m_interactiveItems) {
        if (item && item->isVisible()) {
            interactive.push_back(item->mapRectToScene(QRectF(0, 0, item->width(), item->height())).toAlignedRect());
        }
    }
    const auto region = window_chrome_geometry::hitTest(mouse->position().toPoint(), m_window->size(), 0,
                                                        m_titleBarHeight, QRect(), interactive, m_maximized);
    if (region != window_chrome_geometry::HitRegion::Caption) {
        return QObject::eventFilter(watched, event);
    }
    if (type == QEvent::MouseButtonPress) {
        m_window->startSystemMove();
        return true;
    }
    switch (titleBarDoubleClickAction()) {
    case TitleBarDoubleClick::Zoom:
        toggleMaximize();
        break;
    case TitleBarDoubleClick::Minimize:
        minimize();
        break;
    case TitleBarDoubleClick::None:
        break;
    }
    return true;
}

void WindowChrome::setMaximized(bool value)
{
    if (m_maximized != value) {
        m_maximized = value;
        emit maximizedChanged();
    }
}

void WindowChrome::syncMaximizedFromNative()
{
    if (!m_window) return;
    const Qt::WindowStates states = m_window->windowStates();
    if (!(states & Qt::WindowMinimized)) {
        setMaximized((states & (Qt::WindowMaximized | Qt::WindowFullScreen)) != 0);
    }
}

void WindowChrome::trackNormalGeometry()
{
    if (m_window && nativeStateOf(m_window->windowStates()) == 0) {
        m_normalGeometry = m_window->geometry();
    }
}
