#pragma once

#include <QObject>
#include <QPointer>
#include <QString>

class QQuickWindow;

namespace span::platform::diagnostics {

// Asks Direct3D for its software rasterizer (WARP, "Microsoft Basic Render
// Driver"): slower, but independent of the graphics driver. Qt Quick reads
// the choice once, so call it before the first QQuickWindow exists. Qt's own
// "software" scene graph backend is never used: it cannot draw the
// terminal's custom nodes.
void preferSoftwareRendering();

// Keeps Qt Quick's pipeline cache in `cacheFile` instead of its automatic
// %LOCALAPPDATA% location: the file is loaded, and written back on window
// destruction when `save`. An empty path turns the cache off. Call before the
// window is first exposed; only one process should save to a given file.
void configurePipelineCache(QQuickWindow *window, const QString &cacheFile, bool save);

// The renderer a window ended up with, read after its first frame.
struct RendererFacts {
    QString api;    // "Direct3D 11", "OpenGL", ...
    QString device; // adapter name as the driver reports it
    bool software = false;
};

// The marker file lives from begin() until the first frame or a clean
// destruction; found at the next launch, it means that launch died in the
// driver. An empty path disables it. A scene graph error (not even WARP
// works) exits with -1 for the failure dialog instead of Qt's qFatal.
class GraphicsStartupWatch final : public QObject {
    Q_OBJECT

public:
    explicit GraphicsStartupWatch(QString markerPath, QObject *parent = nullptr);
    ~GraphicsStartupWatch() override;

    // Whether an earlier launch left its marker. Read before begin().
    bool previousStartInterrupted() const;
    void begin();
    void watch(QQuickWindow *window, bool softwarePreferred);

    // Empty until rendererReady().
    RendererFacts renderer() const { return m_renderer; }

signals:
    void rendererReady();

private:
    void finish();

    QString m_markerPath;
    bool m_markerWritten = false;
    bool m_ready = false;
    bool m_failed = false;
    QPointer<QQuickWindow> m_window;
    RendererFacts m_renderer;
};

} // namespace span::platform::diagnostics
