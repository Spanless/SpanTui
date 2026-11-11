#include "application/graphics_mode_service.h"
#include "application/preferences.h"
#include "platform/diagnostics/graphics_startup.h"

#include <QFile>
#include <QQuickGraphicsConfiguration>
#include <QQuickWindow>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QtTest>

#include <memory>

using span::application::GraphicsModeService;
using span::application::Preferences;
using span::platform::diagnostics::configurePipelineCache;
using span::platform::diagnostics::GraphicsStartupWatch;

class GraphicsModeServiceTest final : public QObject {
    Q_OBJECT

private slots:
    void defaultsToHardware();
    void storedSoftwareAppliesAtNextStart();
    void launchOptionHoldsForThisLaunchOnly();
    void interruptedStartStoresSoftware();
    void normalizesUnknownModes();
    void reportsRenderer();
    void startMarkerLivesUntilCleanExit();
    void emptyMarkerPathKeepsNoMarker();
    void pipelineCacheLoadsOnlyAnExistingFile();
};

namespace {

std::unique_ptr<Preferences> openState(const QString &databasePath)
{
    auto state = std::make_unique<Preferences>();
    state->store.setDatabasePathForTesting(databasePath);
    if (!state->open(span::application::SettingsStore::Mode::Workspace)) return nullptr;
    return state;
}

} // namespace

void GraphicsModeServiceTest::defaultsToHardware()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const auto state = openState(dir.filePath(QStringLiteral("state.db")));
    QVERIFY(state);
    const GraphicsModeService graphics(&state->workspace, false, false);
    QCOMPARE(graphics.mode(), QStringLiteral("auto"));
    QVERIFY(!graphics.softwareActive());
    QVERIFY(graphics.softwareReason().isEmpty());
    QVERIFY(!graphics.restartRequired());
    QVERIFY(graphics.fallbackNotice().isEmpty());
}

void GraphicsModeServiceTest::storedSoftwareAppliesAtNextStart()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString databasePath = dir.filePath(QStringLiteral("state.db"));
    {
        const auto state = openState(databasePath);
        QVERIFY(state);
        GraphicsModeService graphics(&state->workspace, false, false);
        QSignalSpy modeChanged(&graphics, &GraphicsModeService::modeChanged);
        graphics.setMode(QStringLiteral("software"));
        QCOMPARE(modeChanged.count(), 1);
        QCOMPARE(state->workspace.renderMode(), QStringLiteral("software"));
        // The process keeps its renderer; the choice waits for a restart.
        QVERIFY(!graphics.softwareActive());
        QVERIFY(graphics.restartRequired());
        state->store.saveNow();
    }
    const auto state = openState(databasePath);
    QVERIFY(state);
    GraphicsModeService graphics(&state->workspace, false, false);
    QVERIFY(graphics.softwareActive());
    QCOMPARE(graphics.softwareReason(), QStringLiteral("setting"));
    QVERIFY(!graphics.restartRequired());

    graphics.setMode(QStringLiteral("auto"));
    QVERIFY(graphics.restartRequired());
}

void GraphicsModeServiceTest::launchOptionHoldsForThisLaunchOnly()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const auto state = openState(dir.filePath(QStringLiteral("state.db")));
    QVERIFY(state);
    const GraphicsModeService graphics(&state->workspace, true, false);
    QVERIFY(graphics.softwareActive());
    QCOMPARE(graphics.softwareReason(), QStringLiteral("launchOption"));
    QCOMPARE(state->workspace.renderMode(), QStringLiteral("auto"));
    QVERIFY(!graphics.restartRequired());
    QVERIFY(graphics.fallbackNotice().isEmpty());
}

void GraphicsModeServiceTest::interruptedStartStoresSoftware()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString databasePath = dir.filePath(QStringLiteral("state.db"));
    {
        const auto state = openState(databasePath);
        QVERIFY(state);
        GraphicsModeService graphics(&state->workspace, false, true);
        QVERIFY(graphics.softwareActive());
        QCOMPARE(graphics.softwareReason(), QStringLiteral("interruptedStart"));
        QCOMPARE(graphics.mode(), QStringLiteral("software"));
        QVERIFY(!graphics.restartRequired());
        QVERIFY(!graphics.fallbackNotice().isEmpty());
    }
    // Stored at once, without waiting for the deferred settings save: a
    // second crash must not lose it.
    const auto state = openState(databasePath);
    QVERIFY(state);
    QCOMPARE(state->workspace.renderMode(), QStringLiteral("software"));
    const GraphicsModeService graphics(&state->workspace, false, false);
    QCOMPARE(graphics.softwareReason(), QStringLiteral("setting"));
    QVERIFY(graphics.fallbackNotice().isEmpty());
}

void GraphicsModeServiceTest::normalizesUnknownModes()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const auto state = openState(dir.filePath(QStringLiteral("state.db")));
    QVERIFY(state);
    state->workspace.setRenderMode(QStringLiteral("software"));
    state->workspace.setRenderMode(QStringLiteral("vulkan"));
    QCOMPARE(state->workspace.renderMode(), QStringLiteral("auto"));
}

void GraphicsModeServiceTest::reportsRenderer()
{
    GraphicsModeService graphics(nullptr, false, false);
    QSignalSpy rendererChanged(&graphics, &GraphicsModeService::rendererChanged);
    graphics.setRenderer(QStringLiteral("Direct3D 11"),
                         QStringLiteral("Microsoft Basic Render Driver"), true);
    QCOMPARE(rendererChanged.count(), 1);
    QCOMPARE(graphics.rendererApi(), QStringLiteral("Direct3D 11"));
    QVERIFY(graphics.rendererSoftware());
    graphics.setRenderer(QStringLiteral("Direct3D 11"),
                         QStringLiteral("Microsoft Basic Render Driver"), true);
    QCOMPARE(rendererChanged.count(), 1);
}

void GraphicsModeServiceTest::startMarkerLivesUntilCleanExit()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString marker = dir.filePath(QStringLiteral("SpanTui.graphics-start"));
    {
        GraphicsStartupWatch watch(marker);
        QVERIFY(!watch.previousStartInterrupted());
        watch.begin();
        QVERIFY(QFile::exists(marker));
    }
    QVERIFY(!QFile::exists(marker));

    // A launch that died keeps its marker; the next one sees it.
    {
        QFile left(marker);
        QVERIFY(left.open(QIODevice::WriteOnly));
    }
    GraphicsStartupWatch next(marker);
    QVERIFY(next.previousStartInterrupted());
}

void GraphicsModeServiceTest::emptyMarkerPathKeepsNoMarker()
{
    GraphicsStartupWatch watch{QString()};
    QVERIFY(!watch.previousStartInterrupted());
    watch.begin();
    QVERIFY(watch.renderer().api.isEmpty());
}

void GraphicsModeServiceTest::pipelineCacheLoadsOnlyAnExistingFile()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString cache = dir.filePath(QStringLiteral("cache/SpanTui.qtpipelinecache"));

    // First launch: nothing to load, so Qt is not pointed at a missing file
    // (it would warn); the cache is still saved at exit.
    QQuickWindow first;
    configurePipelineCache(&first, cache, true);
    QVERIFY(first.graphicsConfiguration().pipelineCacheLoadFile().isEmpty());
    QCOMPARE(first.graphicsConfiguration().pipelineCacheSaveFile(), cache);
    QVERIFY(QFileInfo(cache).absoluteDir().exists());

    {
        QFile saved(cache);
        QVERIFY(saved.open(QIODevice::WriteOnly));
    }
    QQuickWindow next;
    configurePipelineCache(&next, cache, false);
    QCOMPARE(next.graphicsConfiguration().pipelineCacheLoadFile(), cache);
    QVERIFY(next.graphicsConfiguration().pipelineCacheSaveFile().isEmpty());
}

QTEST_MAIN(GraphicsModeServiceTest)
#include "graphics_mode_service_test.moc"
