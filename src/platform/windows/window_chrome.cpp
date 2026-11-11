#include "platform/windows/window_chrome.h"

#include "platform/windows/window_chrome_geometry.h"

#include <QGuiApplication>
#include <QDebug>
#include <QQuickItem>
#include <QQuickWindow>

#include <windows.h>

#include <dwmapi.h>
#include <shellapi.h>
#include <windowsx.h>

#include <atomic>
#include <thread>

#ifndef DWMWA_USE_IMMERSIVE_DARK_MODE
#define DWMWA_USE_IMMERSIVE_DARK_MODE 20
#endif

namespace {

// Windows 10 before 20H1 (1809–1909) rejects DWMWA_USE_IMMERSIVE_DARK_MODE
// (20) and only knows the same switch under its pre-release number.
constexpr DWORD kLegacyImmersiveDarkMode = 19;

using window_chrome_geometry::HitRegion;

int resizeBandWidth(HWND hwnd) {
    const UINT dpi = GetDpiForWindow(hwnd);
    return GetSystemMetricsForDpi(SM_CXSIZEFRAME, dpi) + GetSystemMetricsForDpi(SM_CXPADDEDBORDER, dpi);
}

// Maximized windows cover auto-hide taskbars; leave a one-pixel band on the
// bar's edge so it can still slide out.
unsigned int queryAutoHideTaskbarEdges(HWND hwnd) {
    unsigned int found = 0;
    HMONITOR monitor = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
    MONITORINFO info{};
    info.cbSize = sizeof(info);
    if (!GetMonitorInfoW(monitor, &info)) {
        return found;
    }
    const UINT edges[] = {ABE_LEFT, ABE_TOP, ABE_RIGHT, ABE_BOTTOM};
    for (UINT edge : edges) {
        APPBARDATA bar{};
        bar.cbSize = sizeof(bar);
        bar.uEdge = edge;
        bar.rc = info.rcMonitor;
        if (!SHAppBarMessage(ABM_GETAUTOHIDEBAREX, &bar)) {
            continue;
        }
        found |= 1u << edge;
    }
    return found;
}

void reserveAutoHideTaskbar(unsigned int edges, RECT *client) {
    if (edges & (1u << ABE_LEFT)) client->left += 1;
    if (edges & (1u << ABE_TOP)) client->top += 1;
    if (edges & (1u << ABE_RIGHT)) client->right -= 1;
    if (edges & (1u << ABE_BOTTOM)) client->bottom -= 1;
}

// Pin the client rect to the monitor work area. Frame-metric insets are wrong
// for frameless + WS_THICKFRAME (often too large → visible gaps all around).
bool maximizedClientRect(HWND hwnd, unsigned int autoHideEdges, RECT *client) {
    HMONITOR monitor = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
    MONITORINFO info{};
    info.cbSize = sizeof(info);
    if (!GetMonitorInfoW(monitor, &info)) {
        return false;
    }
    *client = info.rcWork;
    reserveAutoHideTaskbar(autoHideEdges, client);
    return true;
}

LRESULT hitRegionToNative(HitRegion region) {
    switch (region) {
    case HitRegion::Nowhere: return HTNOWHERE;
    case HitRegion::Caption: return HTCAPTION;
    case HitRegion::MaxButton: return HTMAXBUTTON;
    case HitRegion::Left: return HTLEFT;
    case HitRegion::Right: return HTRIGHT;
    case HitRegion::Top: return HTTOP;
    case HitRegion::Bottom: return HTBOTTOM;
    case HitRegion::TopLeft: return HTTOPLEFT;
    case HitRegion::TopRight: return HTTOPRIGHT;
    case HitRegion::BottomLeft: return HTBOTTOMLEFT;
    case HitRegion::BottomRight: return HTBOTTOMRIGHT;
    case HitRegion::Client: break;
    }
    return HTCLIENT;
}

void showNativeSystemMenu(HWND hwnd, int screenX, int screenY) {
    HMENU menu = GetSystemMenu(hwnd, FALSE);
    if (!menu) {
        return;
    }
    const bool zoomed = IsZoomed(hwnd);
    EnableMenuItem(menu, SC_RESTORE, MF_BYCOMMAND | (zoomed ? MF_ENABLED : MF_GRAYED));
    EnableMenuItem(menu, SC_MAXIMIZE, MF_BYCOMMAND | (zoomed ? MF_GRAYED : MF_ENABLED));
    EnableMenuItem(menu, SC_MOVE, MF_BYCOMMAND | (zoomed ? MF_GRAYED : MF_ENABLED));
    EnableMenuItem(menu, SC_SIZE, MF_BYCOMMAND | (zoomed ? MF_GRAYED : MF_ENABLED));
    const int command = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON, screenX, screenY, 0, hwnd, nullptr);
    if (command != 0) {
        PostMessageW(hwnd, WM_SYSCOMMAND, static_cast<WPARAM>(command), 0);
    }
}

} // namespace

struct WindowChrome::AutoHideTaskbarCache {
    std::atomic_uint edges{0};
    std::atomic_bool queryActive{false};
    std::atomic_bool refreshRequested{false};
};

WindowChrome::WindowChrome(QObject *parent)
    : QObject(parent),
      m_autoHideTaskbarCache(std::make_shared<AutoHideTaskbarCache>()) {
}

WindowChrome::~WindowChrome()
{
    if (qGuiApp) {
        qGuiApp->removeNativeEventFilter(this);
    }
}

void WindowChrome::attach(QQuickWindow *window) {
    Q_ASSERT(window);
    m_window = window;
    m_transitionFactsOnly = false;
    HWND hwnd = reinterpret_cast<HWND>(window->winId());
    m_hwnd = hwnd;
    m_lastNativeSizeType = IsIconic(hwnd) ? SIZE_MINIMIZED
        : (IsZoomed(hwnd) ? SIZE_MAXIMIZED : SIZE_RESTORED);
    QMetaObject::invokeMethod(this, &WindowChrome::refreshAutoHideTaskbarEdges,
                              Qt::QueuedConnection);
    connect(window, &QWindow::screenChanged, this, [this]() {
        QMetaObject::invokeMethod(this, &WindowChrome::refreshAutoHideTaskbarEdges,
                                  Qt::QueuedConnection);
    });

    // The filter must be in place before SWP_FRAMECHANGED below: that call
    // sends WM_NCCALCSIZE synchronously, and if default processing answers
    // it the window keeps a native caption (and loses that height at the
    // bottom) until something resizes it.
    qGuiApp->installNativeEventFilter(this);

    // FramelessWindowHint strips system chrome; put the style bits back so
    // SC_MINIMIZE / SC_MAXIMIZE / SC_RESTORE and DWM transitions work.
    // WS_CAPTION stays on: WM_NCCALCSIZE still expands the client over the
    // caption so Windows never paints a second title bar.
    const LONG_PTR style = GetWindowLongPtrW(hwnd, GWL_STYLE);
    SetWindowLongPtrW(hwnd, GWL_STYLE,
                      style | WS_THICKFRAME | WS_CAPTION | WS_MAXIMIZEBOX | WS_MINIMIZEBOX | WS_SYSMENU);

    const MARGINS margins{0, 0, 1, 0};
    DwmExtendFrameIntoClientArea(hwnd, &margins);
    SetWindowPos(hwnd, nullptr, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);

    m_normalGeometry = window->geometry();
    connect(window, &QWindow::windowStateChanged, this, [this](Qt::WindowState) {
        syncMaximizedFromNative();
    });
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
    HWND hwnd = reinterpret_cast<HWND>(window->winId());
    m_hwnd = hwnd;
    m_lastNativeSizeType = IsIconic(hwnd) ? SIZE_MINIMIZED
        : (IsZoomed(hwnd) ? SIZE_MAXIMIZED : SIZE_RESTORED);
    qGuiApp->installNativeEventFilter(this);
}

QRect WindowChrome::normalGeometry() const {
    // Live geometry while restored; last tracked rect while zoomed/minimized.
    if (m_window && m_hwnd) {
        HWND hwnd = static_cast<HWND>(m_hwnd);
        if (!IsZoomed(hwnd) && !IsIconic(hwnd)) {
            return m_window->geometry();
        }
    }
    return m_normalGeometry;
}

bool WindowChrome::maximized() const {
    return m_maximized;
}

bool WindowChrome::effectivelyMaximized() const {
    if (!m_hwnd) {
        return m_maximized;
    }
    HWND hwnd = static_cast<HWND>(m_hwnd);
    if (IsZoomed(hwnd)) {
        return true;
    }
    WINDOWPLACEMENT placement{};
    placement.length = sizeof(placement);
    if (GetWindowPlacement(hwnd, &placement)) {
        if (placement.showCmd == SW_SHOWMAXIMIZED) {
            return true;
        }
        // Minimized from a maximized window — restore should go back to max.
        if (placement.showCmd == SW_SHOWMINIMIZED && (placement.flags & WPF_RESTORETOMAXIMIZED)) {
            return true;
        }
    }
    return false;
}

void WindowChrome::minimize() {
    if (!m_hwnd) {
        return;
    }
    PostMessageW(static_cast<HWND>(m_hwnd), WM_SYSCOMMAND, SC_MINIMIZE, 0);
}

void WindowChrome::toggleMaximize() {
    if (!m_hwnd) {
        return;
    }
    HWND hwnd = static_cast<HWND>(m_hwnd);
    if (IsZoomed(hwnd)) {
        PostMessageW(hwnd, WM_SYSCOMMAND, SC_RESTORE, 0);
    } else {
        // Snapshot restore size before the system zooms (placement backup).
        if (m_window && !IsIconic(hwnd)) {
            m_normalGeometry = m_window->geometry();
        }
        PostMessageW(hwnd, WM_SYSCOMMAND, SC_MAXIMIZE, 0);
    }
}

void WindowChrome::requestClose() {
    if (m_window) {
        m_window->close();
    }
}

void WindowChrome::setDarkFrame(bool dark) {
    if (!m_hwnd) {
        return;
    }
    const BOOL value = dark ? TRUE : FALSE;
    const HWND hwnd = static_cast<HWND>(m_hwnd);
    if (FAILED(DwmSetWindowAttribute(hwnd, DWMWA_USE_IMMERSIVE_DARK_MODE, &value, sizeof(value)))) {
        DwmSetWindowAttribute(hwnd, kLegacyImmersiveDarkMode, &value, sizeof(value));
    }
}

void WindowChrome::setTitleBarHeight(int logicalHeight) {
    m_titleBarHeight = logicalHeight;
}

void WindowChrome::addInteractiveItem(QQuickItem *item) {
    if (item && !m_interactiveItems.contains(item)) {
        m_interactiveItems.push_back(item);
    }
}

void WindowChrome::setMaximized(bool value) {
    if (m_maximized != value) {
        m_maximized = value;
        emit maximizedChanged();
    }
}

void WindowChrome::syncMaximizedFromNative() {
    if (!m_hwnd) {
        return;
    }
    setMaximized(IsZoomed(static_cast<HWND>(m_hwnd)) != FALSE);
}

void WindowChrome::trackNormalGeometry() {
    if (!m_window || !m_hwnd) {
        return;
    }
    HWND hwnd = static_cast<HWND>(m_hwnd);
    if (!IsZoomed(hwnd) && !IsIconic(hwnd)) {
        m_normalGeometry = m_window->geometry();
    }
}

QRect WindowChrome::physicalItemRect(const QQuickItem *item) const {
    if (!item || !item->isVisible() || !m_window) {
        return QRect();
    }
    const QRectF scene = item->mapRectToScene(QRectF(0, 0, item->width(), item->height()));
    const qreal dpr = m_window->devicePixelRatio();
    return QRectF(scene.x() * dpr, scene.y() * dpr, scene.width() * dpr, scene.height() * dpr).toRect();
}

bool WindowChrome::nativeEventFilter(const QByteArray &eventType, void *message, qintptr *result) {
    if (eventType != "windows_generic_MSG" || !m_window || !m_hwnd) {
        return false;
    }
    MSG *msg = static_cast<MSG *>(message);
    HWND hwnd = static_cast<HWND>(m_hwnd);
    if (msg->hwnd != hwnd) {
        return false;
    }

    if (m_transitionFactsOnly) {
        switch (msg->message) {
        case WM_SIZE:
            if ((msg->wParam == SIZE_MAXIMIZED
                 || msg->wParam == SIZE_RESTORED
                 || msg->wParam == SIZE_MINIMIZED)
                && msg->wParam != m_lastNativeSizeType) {
                m_lastNativeSizeType = static_cast<unsigned int>(msg->wParam);
                emit nativeWindowStateChanged(static_cast<int>(msg->wParam));
            }
            return false;
        case WM_ENTERSIZEMOVE:
            emit interactiveResizeStarted();
            return false;
        case WM_EXITSIZEMOVE:
            emit interactiveResizeEnded();
            return false;
        case WM_DPICHANGED:
            emit devicePixelRatioChanged();
            return false;
        default:
            return false;
        }
    }

    switch (msg->message) {
    case WM_NCCALCSIZE: {
        if (msg->wParam != TRUE) {
            return false;
        }
        auto *params = reinterpret_cast<NCCALCSIZE_PARAMS *>(msg->lParam);
        RECT *client = &params->rgrc[0];
        if (IsZoomed(hwnd)) {
            maximizedClientRect(
                hwnd,
                m_autoHideTaskbarCache->edges.load(std::memory_order_acquire),
                client);
        }
        // Leaving the rect untouched otherwise removes the non-client frame;
        // hit testing below supplies resize and drag behavior.
        *result = 0;
        return true;
    }

    case WM_SIZE:
        // Keep the caption icon in sync after SC_MAXIMIZE / SC_RESTORE / min.
        if ((msg->wParam == SIZE_MAXIMIZED || msg->wParam == SIZE_RESTORED
             || msg->wParam == SIZE_MINIMIZED)
            && msg->wParam != m_lastNativeSizeType) {
            m_lastNativeSizeType = static_cast<unsigned int>(msg->wParam);
            syncMaximizedFromNative();
            emit nativeWindowStateChanged(static_cast<int>(msg->wParam));
        }
        return false;

    case WM_ENTERSIZEMOVE:
        emit interactiveResizeStarted();
        return false;

    case WM_EXITSIZEMOVE:
        emit interactiveResizeEnded();
        QMetaObject::invokeMethod(this, &WindowChrome::refreshAutoHideTaskbarEdges,
                                  Qt::QueuedConnection);
        return false;

    case WM_DPICHANGED:
        emit devicePixelRatioChanged();
        return false;

    case WM_NCHITTEST: {
        POINT pt{GET_X_LPARAM(msg->lParam), GET_Y_LPARAM(msg->lParam)};
        ScreenToClient(hwnd, &pt);
        RECT rc{};
        GetClientRect(hwnd, &rc);
        const qreal dpr = m_window->devicePixelRatio();

        QVector<QRect> interactive;
        interactive.reserve(m_interactiveItems.size());
        for (const auto &item : m_interactiveItems) {
            const QRect rect = physicalItemRect(item);
            if (rect.isValid()) {
                interactive.push_back(rect);
            }
        }

        const HitRegion region = window_chrome_geometry::hitTest(
            QPoint(pt.x, pt.y),
            QSize(rc.right - rc.left, rc.bottom - rc.top),
            resizeBandWidth(hwnd),
            qRound(m_titleBarHeight * dpr),
            QRect(),
            interactive,
            IsZoomed(hwnd) != FALSE);

        if (region == HitRegion::Client) {
            return false;
        }
        // HTNOWHERE lets the system route the click to whatever is under us
        // (taskbar, desktop) instead of eating it in our maximized overhang.
        *result = hitRegionToNative(region);
        return true;
    }

    case WM_NCRBUTTONUP:
        if (msg->wParam == HTCAPTION) {
            showNativeSystemMenu(hwnd, GET_X_LPARAM(msg->lParam), GET_Y_LPARAM(msg->lParam));
            *result = 0;
            return true;
        }
        return false;

    // lParam = -1 keeps DefWindowProc from repainting a classic non-client
    // frame on activation changes.
    case WM_NCACTIVATE:
        *result = DefWindowProcW(hwnd, WM_NCACTIVATE, msg->wParam, -1);
        return true;

    default:
        return false;
    }
}

void WindowChrome::refreshAutoHideTaskbarEdges()
{
    const HWND hwnd = static_cast<HWND>(m_hwnd);
    if (!hwnd) {
        m_autoHideTaskbarCache->edges.store(0, std::memory_order_release);
        return;
    }
    const auto cache = m_autoHideTaskbarCache;
    cache->refreshRequested.store(true, std::memory_order_release);
    bool expected = false;
    if (!cache->queryActive.compare_exchange_strong(
            expected, true, std::memory_order_acq_rel)) {
        return;
    }
    std::thread([cache, hwnd]() {
        while (true) {
            cache->refreshRequested.store(false, std::memory_order_release);
            cache->edges.store(queryAutoHideTaskbarEdges(hwnd),
                               std::memory_order_release);
            cache->queryActive.store(false, std::memory_order_release);
            if (!cache->refreshRequested.load(std::memory_order_acquire)) {
                return;
            }
            bool idle = false;
            if (!cache->queryActive.compare_exchange_strong(
                    idle, true, std::memory_order_acq_rel)) {
                return;
            }
        }
    }).detach();
}
