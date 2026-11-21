#include "platform/diagnostics/graphics_startup.h"

#include "platform/graphics_adapter.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDebug>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QQuickGraphicsConfiguration>
#include <QQuickWindow>
#include <QSGRendererInterface>

#include <utility>

namespace span::platform::diagnostics {
namespace {

QString apiName(QSGRendererInterface::GraphicsApi api)
{
    switch (api) {
    case QSGRendererInterface::Direct3D11:
        return QStringLiteral("Direct3D 11");
    case QSGRendererInterface::Direct3D12:
        return QStringLiteral("Direct3D 12");
    case QSGRendererInterface::OpenGL:
        return QStringLiteral("OpenGL");
    case QSGRendererInterface::Vulkan:
        return QStringLiteral("Vulkan");
    case QSGRendererInterface::Metal:
        return QStringLiteral("Metal");
    case QSGRendererInterface::Software:
        return QStringLiteral("Qt Quick software");
    default:
        return QStringLiteral("unknown");
    }
}

} // namespace

void preferSoftwareRendering()
{
    qputenv("QSG_RHI_PREFER_SOFTWARE_RENDERER", "1");
}

void configurePipelineCache(QQuickWindow *window, const QString &cacheFile, bool save)
{
    if (!window)
        return;
    QQuickGraphicsConfiguration config = window->graphicsConfiguration();
    config.setAutomaticPipelineCache(false);
    if (!cacheFile.isEmpty()) {
        QDir().mkpath(QFileInfo(cacheFile).absolutePath());
        // Qt warns when the load file cannot be opened; there is none until
        // the first exit saves one, or after the cache was deleted.
        if (QFileInfo::exists(cacheFile)) config.setPipelineCacheLoadFile(cacheFile);
        if (save)
            config.setPipelineCacheSaveFile(cacheFile);
    }
    window->setGraphicsConfiguration(config);
}

GraphicsStartupWatch::GraphicsStartupWatch(QString markerPath, QObject *parent)
    : QObject(parent)
    , m_markerPath(std::move(markerPath))
{
}

GraphicsStartupWatch::~GraphicsStartupWatch()
{
    // Reaching the destructor is a clean exit, whether or not a frame was
    // drawn: only a launch that dies keeps its marker.
    if (m_markerWritten)
        QFile::remove(m_markerPath);
}

bool GraphicsStartupWatch::previousStartInterrupted() const
{
    return !m_markerPath.isEmpty() && QFile::exists(m_markerPath);
}

void GraphicsStartupWatch::begin()
{
    if (m_markerPath.isEmpty())
        return;
    QFile marker(m_markerPath);
    if (!marker.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        qWarning().noquote() << "Cannot write the graphics start marker" << m_markerPath
                             << marker.errorString();
        return;
    }
    marker.write(QDateTime::currentDateTimeUtc().toString(Qt::ISODate).toUtf8());
    m_markerWritten = true;
}

void GraphicsStartupWatch::watch(QQuickWindow *window, bool softwarePreferred)
{
    m_window = window;
    // With the threaded render loop both signals arrive from the render
    // thread; queued, they run here on the GUI thread. A connected
    // sceneGraphError also replaces Qt's default qFatal.
    connect(window, &QQuickWindow::frameSwapped, this, &GraphicsStartupWatch::finish,
            Qt::QueuedConnection);
    connect(window, &QQuickWindow::sceneGraphError, this,
            [this, softwarePreferred](QQuickWindow::SceneGraphError, const QString &message) {
                if (m_failed)
                    return;
                m_failed = true;
                qCritical().noquote()
                    << (softwarePreferred
                            ? tr("SpanTui could not start its graphics, not even with software "
                                 "rendering: %1. Update the graphics driver; on a remote desktop "
                                 "or virtual machine, turn on its graphics support.")
                            : tr("SpanTui could not start its graphics: %1. Update the graphics "
                                 "driver, or start SpanTui with --safe-graphics to use software "
                                 "rendering."))
                           .arg(message);
                QCoreApplication::exit(-1);
            },
            Qt::QueuedConnection);
}

void GraphicsStartupWatch::finish()
{
    if (m_ready)
        return;
    m_ready = true;
    if (m_window)
        disconnect(m_window, &QQuickWindow::frameSwapped, this, &GraphicsStartupWatch::finish);
    if (m_markerWritten) {
        QFile::remove(m_markerPath);
        m_markerWritten = false;
    }
    if (m_window) {
        if (QSGRendererInterface *renderer = m_window->rendererInterface()) {
            m_renderer.api = apiName(renderer->graphicsApi());
            const GraphicsAdapter adapter = graphicsAdapter(m_window);
            m_renderer.device = adapter.name;
            m_renderer.software = adapter.software || renderer->graphicsApi() == QSGRendererInterface::Software;
        }
    }
    const QString device = m_renderer.device.isEmpty() ? QStringLiteral("an unknown adapter") : m_renderer.device;
    qInfo().noquote() << "Graphics:" << m_renderer.api << "on" << device
                      << (m_renderer.software ? "(software)" : "(hardware)");
    emit rendererReady();
}

} // namespace span::platform::diagnostics
