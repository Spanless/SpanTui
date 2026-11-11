#include "platform/diagnostics/graphics_startup.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDebug>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QQuickGraphicsConfiguration>
#include <QQuickWindow>
#include <QSGRendererInterface>

#ifdef Q_OS_WIN
#include <d3d11.h>
#include <dxgi.h>
#endif

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

#ifdef Q_OS_WIN
// The adapter behind a Direct3D 11 window, through the public device
// handle. WARP reports Microsoft's vendor id and the Basic Render Driver id.
void readDirect3D11Adapter(QSGRendererInterface *renderer, QQuickWindow *window,
                           RendererFacts *facts)
{
    auto *device = static_cast<ID3D11Device *>(
        renderer->getResource(window, QSGRendererInterface::DeviceResource));
    if (!device)
        return;
    IDXGIDevice *dxgiDevice = nullptr;
    if (FAILED(device->QueryInterface(__uuidof(IDXGIDevice),
                                      reinterpret_cast<void **>(&dxgiDevice))))
        return;
    IDXGIAdapter *adapter = nullptr;
    if (SUCCEEDED(dxgiDevice->GetAdapter(&adapter))) {
        DXGI_ADAPTER_DESC description{};
        if (SUCCEEDED(adapter->GetDesc(&description))) {
            facts->device = QString::fromWCharArray(description.Description);
            facts->software = description.VendorId == 0x1414 && description.DeviceId == 0x8c;
        }
        adapter->Release();
    }
    dxgiDevice->Release();
}
#endif

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
#ifdef Q_OS_WIN
            if (renderer->graphicsApi() == QSGRendererInterface::Direct3D11)
                readDirect3D11Adapter(renderer, m_window, &m_renderer);
#endif
            if (renderer->graphicsApi() == QSGRendererInterface::Software)
                m_renderer.software = true;
        }
    }
    qInfo().noquote() << "Graphics:" << m_renderer.api << "on" << m_renderer.device
                      << (m_renderer.software ? "(software)" : "(hardware)");
    emit rendererReady();
}

} // namespace span::platform::diagnostics
