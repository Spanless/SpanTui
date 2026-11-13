// Renders real TerminalViews offscreen at a fixed tick rate standing in for
// vsync. Each line starts with a marker cell whose colour encodes its
// number, so lag and freezes are measured on the rendered pixels.
//
// Not in CTest; run by hand (docs/workflow/testing.md):
//   <build>\terminal_render_benchmark.exe [--hz 144] [--scale 1.5]
//       [--size 1600x960] [--history 90000] [--ticks 480]
//       [--scenario <name>]... [--json <file>] [--list]
// Offscreen fonts go through FreeType, not DirectWrite: compare runs with
// each other, not with the product window.
#include "terminal/api/terminal_process.h"
#include "terminal/api/terminal_session.h"
#include "terminal/core/terminal_viewport_trace.h"
#include "terminal/render/terminal_view.h"
#include "terminal/terminal_constants.h"

#include <QCommandLineParser>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QFontDatabase>
#include <QGuiApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QQuickRenderControl>
#include <QQuickRenderTarget>
#include <QQuickWindow>
#include <QSGNode>
#include <QSGRendererInterface>
#include <QTimer>
#include <private/qquickitem_p.h>
#include <rhi/qrhi.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <functional>
#include <memory>
#include <mutex>
#include <numeric>
#include <vector>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

using namespace span::terminal;

namespace {

// Opaque and far outside every marker value, so cleared pixels never decode.
const QColor kWindowColor(0x20, 0x20, 0x20);
constexpr double kPi = 3.14159265358979323846;

double processCpuSeconds()
{
    FILETIME creation{}, exit{}, kernel{}, user{};
    if (!GetProcessTimes(GetCurrentProcess(), &creation, &exit, &kernel, &user)) {
        return 0.0;
    }
    ULARGE_INTEGER kernelTicks{};
    kernelTicks.LowPart = kernel.dwLowDateTime;
    kernelTicks.HighPart = kernel.dwHighDateTime;
    ULARGE_INTEGER userTicks{};
    userTicks.LowPart = user.dwLowDateTime;
    userTicks.HighPart = user.dwHighDateTime;
    return double(kernelTicks.QuadPart + userTicks.QuadPart) / 10000000.0;
}

// The child of a benchmark session: output is pushed by the scenario.
class ScriptedProcess final : public TerminalProcess {
public:
    bool start(const StartOptions &, OutputCallback outputCallback, ExitCallback, QString *) override
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_output = std::move(outputCallback);
        m_running.store(true, std::memory_order_release);
        return true;
    }
    bool write(const QByteArray &, TerminalInputKind) override { return isRunning(); }
    bool cancelPendingWritesAndInterrupt() override { return isRunning(); }
    void requestResize(int, int) override {}
    void stop() override { m_running.store(false, std::memory_order_release); }
    bool isRunning() const override { return m_running.load(std::memory_order_acquire); }
    qsizetype pendingWriteBytes() const override { return 0; }
    quint32 processId() const override { return isRunning() ? 42 : 0; }
    QList<quint32> jobProcessIds() const override { return {}; }

    void emitOutput(const QByteArray &bytes)
    {
        OutputCallback output;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            output = m_output;
        }
        if (output) output(bytes, true);
    }

private:
    mutable std::mutex m_mutex;
    OutputCallback m_output;
    std::atomic_bool m_running{false};
};

// A background colour that decodes back to `value`; offset by one so that
// black (0) is never a marker.
QByteArray marker(int value)
{
    const int encoded = value + 1;
    return QByteArrayLiteral("\x1b[48;2;") + QByteArray::number((encoded >> 16) & 0xff) + ';'
        + QByteArray::number((encoded >> 8) & 0xff) + ';' + QByteArray::number(encoded & 0xff)
        + QByteArrayLiteral("m \x1b[49m");
}

int decodeMarker(const uchar *pixel)
{
    if (pixel[3] != 0xff) return -1;
    return ((int(pixel[0]) << 16) | (int(pixel[1]) << 8) | int(pixel[2])) - 1;
}

// A main-screen history line as a shell, compiler or log would print it. All
// variants stay well under 160 columns so no line wraps: history index and
// line number stay equal.
QByteArray historyLine(int number)
{
    QByteArray body;
    switch (number % 8) {
    case 0:
        body = QByteArrayLiteral("\x1b[1;32mspan@workstation\x1b[0m:\x1b[1;34m~/projects/span-tui/src/terminal\x1b[0m$ "
                                 "cargo build --release --workspace --all-features");
        break;
    case 1:
        body = QByteArrayLiteral("\x1b[2m[2026-10-28 09:41:") + QByteArray::number(number % 60).rightJustified(2, '0')
            + QByteArrayLiteral("]\x1b[0m \x1b[36mINFO \x1b[0m render::scene  prepared frame ")
            + QByteArray::number(number) + QByteArrayLiteral(" rows=56 glyphs=11520 cost=1.42ms");
        break;
    case 2:
        body = QByteArrayLiteral(
            "\x1b[31m-    let layout = TerminalRowLayoutBuilder::build(line, fonts, foreground, background);");
        break;
    case 3:
        body = QByteArrayLiteral("\x1b[32m+    let layout = row_layout::build(&line, &style, PaletteMode::Resolved, ")
            + QByteArray::number(number) + QByteArrayLiteral(");");
        break;
    case 4:
        body = QStringLiteral(
                   "\x1b[33m警告\x1b[0m：第 %1 行的字形缓存未命中，已回退到「Span Mono」并重新整形（耗时 0.8 毫秒）")
                   .arg(number)
                   .toUtf8();
        break;
    case 5:
        body = QStringLiteral("│ ├── src/terminal/render/terminal_scene_graph.cpp │ %1 │ ██████░░░░ 62% │ ⣿⣷⣶⣦⣤⣀ │")
                   .arg(number, 6)
                   .toUtf8();
        break;
    case 6:
        for (int segment = 0; segment < 10; ++segment) {
            const int shade = (number * 7 + segment * 23) % 200;
            body += QByteArrayLiteral("\x1b[48;2;") + QByteArray::number(30 + shade / 2) + ';'
                + QByteArray::number(40 + segment * 9) + ';' + QByteArray::number(60 + shade / 3)
                + QByteArrayLiteral("m segment-") + QByteArray::number(segment).rightJustified(2, '0')
                + QByteArrayLiteral(" \x1b[0m");
        }
        break;
    default:
        body = QByteArrayLiteral("    at span::terminal::TerminalFramePreparer::prepare (terminal_frame_preparer.cpp:")
            + QByteArray::number(number) + QByteArrayLiteral(") \x1b[3mitalic note\x1b[0m");
        break;
    }
    return marker(number) + body + QByteArrayLiteral("\x1b[0m\r\n");
}

enum class ChurnContent { Text, Box, Cjk };

// One full alternate-screen frame: every row restarts with the frame's
// marker (a torn frame shows two values) and fills the width in 16-column
// runs of one colour. CJK text scatters 3500 characters, about as many as
// everyday Chinese uses, differently every frame: the first frames
// rasterize them (a screen of distinct text at once), the rest look them up.
QByteArray churnFrame(int frame, int columns, int rows, ChurnContent content)
{
    static const QString box = QStringLiteral("─│┌┐└┘├┤┬┴┼═║╔╗╚╝");
    static const QString blocks = QStringLiteral("▁▂▃▄▅▆▇█▌▐░▒▓");
    QByteArray bytes = QByteArrayLiteral("\x1b[?25l");
    for (int row = 0; row < rows; ++row) {
        bytes += QByteArrayLiteral("\x1b[") + QByteArray::number(row + 1) + QByteArrayLiteral(";1H");
        bytes += marker(frame);
        for (int column = 1; column < columns; column += 16) {
            const int seed = frame * 7 + row * 13 + column;
            bytes += QByteArrayLiteral("\x1b[38;2;") + QByteArray::number(80 + seed % 170) + ';'
                + QByteArray::number(120 + (seed * 3) % 130) + ';' + QByteArray::number(90 + (seed * 5) % 160) + 'm';
            QString run;
            for (int offset = 0; offset < 16 && column + offset < columns; ++offset) {
                const int value = seed + offset;
                if (content == ChurnContent::Text) {
                    run += QChar(u'!' + value % 94);
                } else if (content == ChurnContent::Cjk) {
                    // Wide: one character every two columns.
                    if (offset % 2 == 0 && column + offset + 1 < columns) {
                        const qint64 cell = qint64(row) * columns + column + offset;
                        run += QChar(char16_t(0x4E00 + (frame * 7919 + cell * 104729) % 3500));
                    }
                } else if (value % 3 == 0) {
                    run += box.at(value % box.size());
                } else if (value % 3 == 1) {
                    run += blocks.at(value % blocks.size());
                } else {
                    run += QChar(char16_t(0x2800 + value % 256));
                }
            }
            bytes += run.toUtf8();
        }
    }
    return bytes + QByteArrayLiteral("\x1b[0m");
}

struct Terminal {
    std::atomic<ScriptedProcess *> process{nullptr};
    std::unique_ptr<TerminalSession> session;
    TerminalView *view = nullptr;
    int lineCount = 0;
};

struct FrameTiming {
    double polishMs = 0;
    double syncMs = 0;
    double renderMs = 0;
};

class OffscreenScene final {
public:
    ~OffscreenScene()
    {
        // Render target resources belong to the control's QRhi, and the
        // control releases the scene graph before its window goes.
        m_pass.reset();
        m_target.reset();
        m_depth.reset();
        m_texture.reset();
        m_control.reset();
        m_window.reset();
    }

    bool initialize(const QSize &size, QString *error)
    {
        m_control = std::make_unique<QQuickRenderControl>();
        m_window = std::make_unique<QQuickWindow>(m_control.get());
        m_window->setColor(kWindowColor);
        m_window->resize(size);
        m_window->contentItem()->setSize(size);
        // Terminal views count a hidden window as "nothing to present".
        // The offscreen platform shows it without any desktop surface.
        m_window->show();
        if (!m_control->initialize()) {
            *error = QStringLiteral("QQuickRenderControl::initialize failed");
            return false;
        }
        QRhi *rhi = m_control->rhi();
        if (!rhi) {
            *error = QStringLiteral("no QRhi");
            return false;
        }
        m_pixelSize = size * m_window->devicePixelRatio();
        m_texture.reset(rhi->newTexture(QRhiTexture::RGBA8, m_pixelSize, 1,
                                        QRhiTexture::RenderTarget | QRhiTexture::UsedAsTransferSource));
        m_depth.reset(rhi->newRenderBuffer(QRhiRenderBuffer::DepthStencil, m_pixelSize, 1));
        if (!m_texture->create() || !m_depth->create()) {
            *error = QStringLiteral("cannot create the render target");
            return false;
        }
        QRhiTextureRenderTargetDescription description{QRhiColorAttachment(m_texture.get())};
        description.setDepthStencilBuffer(m_depth.get());
        m_target.reset(rhi->newTextureRenderTarget(description));
        m_pass.reset(m_target->newCompatibleRenderPassDescriptor());
        m_target->setRenderPassDescriptor(m_pass.get());
        if (!m_target->create()) {
            *error = QStringLiteral("cannot create the texture render target");
            return false;
        }
        m_window->setRenderTarget(QQuickRenderTarget::fromRhiRenderTarget(m_target.get()));
        QObject::connect(m_control.get(), &QQuickRenderControl::renderRequested, [this]() { m_dirty = true; });
        QObject::connect(m_control.get(), &QQuickRenderControl::sceneChanged, [this]() { m_dirty = true; });
        m_dirty = true;
        return true;
    }

    QQuickWindow *window() const { return m_window.get(); }
    qreal devicePixelRatio() const { return m_window->devicePixelRatio(); }
    bool dirty() const { return m_dirty; }

    QString describeRenderer() const
    {
        QRhi *rhi = m_control->rhi();
        return rhi ? QStringLiteral("%1 (%2)").arg(QString::fromLatin1(rhi->backendName()),
                                                   QString::fromUtf8(rhi->driverInfo().deviceName))
                   : QString();
    }

    // Renders one frame and reads back one-pixel columns at `columnsX`
    // (device pixels), top row first.
    FrameTiming render(const QVector<int> &columnsX, QVector<QByteArray> *columns)
    {
        m_dirty = false;
        FrameTiming timing;
        QElapsedTimer clock;
        clock.start();
        m_control->polishItems();
        timing.polishMs = clock.nsecsElapsed() / 1e6;
        clock.restart();
        m_control->beginFrame();
        m_control->sync();
        timing.syncMs = clock.nsecsElapsed() / 1e6;
        clock.restart();
        m_control->render();
        QRhi *rhi = m_control->rhi();
        std::vector<QRhiReadbackResult> results(size_t(columnsX.size()));
        QRhiResourceUpdateBatch *batch = rhi->nextResourceUpdateBatch();
        for (int index = 0; index < columnsX.size(); ++index) {
            QRhiReadbackDescription readback(m_texture.get());
            readback.setRect(
                QRect(std::clamp(columnsX.at(index), 0, m_pixelSize.width() - 1), 0, 1, m_pixelSize.height()));
            batch->readBackTexture(readback, &results[size_t(index)]);
        }
        m_control->commandBuffer()->resourceUpdate(batch);
        m_control->endFrame();
        timing.renderMs = clock.nsecsElapsed() / 1e6;
        columns->clear();
        for (QRhiReadbackResult &result : results) {
            QByteArray column = std::move(result.data);
            if (rhi->isYUpInFramebuffer()) {
                QByteArray flipped(column.size(), Qt::Uninitialized);
                const int rows = int(column.size() / 4);
                for (int row = 0; row < rows; ++row) {
                    std::copy_n(column.constData() + (rows - 1 - row) * 4, 4, flipped.data() + row * 4);
                }
                column = std::move(flipped);
            }
            columns->push_back(std::move(column));
        }
        return timing;
    }

private:
    std::unique_ptr<QQuickRenderControl> m_control;
    std::unique_ptr<QQuickWindow> m_window;
    QSize m_pixelSize;
    std::unique_ptr<QRhiTexture> m_texture;
    std::unique_ptr<QRhiRenderBuffer> m_depth;
    std::unique_ptr<QRhiTextureRenderTarget> m_target;
    std::unique_ptr<QRhiRenderPassDescriptor> m_pass;
    bool m_dirty = false;
};

void countNodes(const QSGNode *node, int *total, int *geometry)
{
    for (const QSGNode *child = node ? node->firstChild() : nullptr; child; child = child->nextSibling()) {
        ++*total;
        if (child->type() == QSGNode::GeometryNodeType) ++*geometry;
        countNodes(child, total, geometry);
    }
}

double percentile(QVector<double> values, double fraction)
{
    if (values.isEmpty()) return 0;
    std::sort(values.begin(), values.end());
    const qsizetype index = std::min<qsizetype>(values.size() - 1, qsizetype(std::ceil(fraction * values.size())) - 1);
    return values.at(std::max<qsizetype>(0, index));
}

// What one tick of a scenario measured. `expected` is the value the
// picture should show (top line number or frame number), `presented` the
// one it shows; -1 when the picture has no marker there.
struct Sample {
    bool rendered = false;
    FrameTiming timing;
    QVector<int> expected;
    QVector<int> presented;
    bool torn = false;
};

class Benchmark final {
public:
    struct Options {
        int hz = 144;
        QSize size{1600, 960};
        int history = 90000;
        int ticks = 480;
        QFont font;
    };

    explicit Benchmark(Options options) : m_options(std::move(options)) {}

    QJsonObject meta;

    bool runScenario(const QString &name, QJsonArray *results)
    {
        // A wheel notch about every 50 ms, then continuous scrolling.
        if (name == QLatin1String("scroll-notches")) return scroll(name, 3, 7, results);
        if (name == QLatin1String("scroll-slow")) return scroll(name, 3, 1, results);
        if (name == QLatin1String("scroll-fast")) return scroll(name, 15, 1, results);
        if (name == QLatin1String("scroll-fling")) return scroll(name, 60, 1, results);
        if (name == QLatin1String("scrollbar-drag")) return scrollbarDrag(name, results);
        if (name == QLatin1String("scrollbar-drag-flood")) return scrollbarDragFlood(name, results);
        if (name == QLatin1String("log-flood")) return logFlood(name, results);
        if (name == QLatin1String("churn-text")) return churn(name, ChurnContent::Text, 1, results);
        if (name == QLatin1String("churn-box")) return churn(name, ChurnContent::Box, 1, results);
        if (name == QLatin1String("churn-cjk")) return churn(name, ChurnContent::Cjk, 1, results);
        if (name == QLatin1String("multi-churn")) return churn(name, ChurnContent::Text, 4, results);
        qCritical().noquote() << "unknown scenario" << name;
        return false;
    }

    static QStringList scenarios()
    {
        return {
            QStringLiteral("scroll-notches"), QStringLiteral("scroll-slow"),    QStringLiteral("scroll-fast"),
            QStringLiteral("scroll-fling"),   QStringLiteral("scrollbar-drag"), QStringLiteral("scrollbar-drag-flood"),
            QStringLiteral("log-flood"),      QStringLiteral("churn-text"),     QStringLiteral("churn-box"),
            QStringLiteral("churn-cjk"),      QStringLiteral("multi-churn")};
    }

private:
    // One window with `count` terminals tiled in a grid.
    bool setUp(int count)
    {
        if (m_scene && int(m_terminals.size()) == count) return true;
        tearDown();
        m_scene = std::make_unique<OffscreenScene>();
        QString error;
        if (!m_scene->initialize(m_options.size, &error)) {
            qCritical().noquote() << "render setup failed:" << error;
            return false;
        }
        const int gridColumns = count > 1 ? 2 : 1;
        const int gridRows = (count + gridColumns - 1) / gridColumns;
        const QSizeF cell(qreal(m_options.size.width()) / gridColumns, qreal(m_options.size.height()) / gridRows);
        for (int index = 0; index < count; ++index) {
            auto terminal = std::make_unique<Terminal>();
            Terminal *raw = terminal.get();
            terminal->session = std::make_unique<TerminalSession>(
                QStringLiteral("render-benchmark-%1").arg(index), QDir::currentPath(),
                QVariantMap{{QStringLiteral("columns"), 120}, {QStringLiteral("rows"), 32}}, nullptr,
                [raw]() -> std::unique_ptr<TerminalProcess> {
                    auto process = std::make_unique<ScriptedProcess>();
                    raw->process.store(process.get(), std::memory_order_release);
                    return process;
                });
            terminal->view = new TerminalView(m_scene->window()->contentItem());
            TerminalView *view = terminal->view;
            view->setPosition(QPointF((index % gridColumns) * cell.width(), (index / gridColumns) * cell.height()));
            view->setSize(cell);
            view->setFont(m_options.font);
            view->setForegroundColor(QColor(0xd8, 0xd8, 0xd8));
            view->setBackgroundColor(Qt::transparent);
            view->setMaximumFrameRate(m_options.hz);
            view->setSession(terminal->session.get());
            m_terminals.push_back(std::move(terminal));
        }
        for (const auto &terminal : m_terminals) {
            if (!terminal->session->start()) {
                qCritical() << "session failed to start";
                return false;
            }
        }
        // The view commits its viewport once it has metrics.
        const bool ready = waitUntil(
            [this]() {
                for (const auto &terminal : m_terminals) {
                    if (!terminal->session->running() || !terminal->process.load(std::memory_order_acquire)
                        || terminal->session->columns() < 60) {
                        return false;
                    }
                }
                return true;
            },
            10000);
        if (!ready) {
            qCritical() << "terminals did not reach their viewport size";
            return false;
        }
        const Terminal &first = *m_terminals.front();
        meta.insert(QStringLiteral("renderer"), m_scene->describeRenderer());
        meta.insert(QStringLiteral("devicePixelRatio"), m_scene->devicePixelRatio());
        meta.insert(QStringLiteral("cellWidth"), first.view->cellWidth());
        meta.insert(QStringLiteral("cellHeight"), first.view->cellHeight());
        meta.insert(QStringLiteral("columns%1").arg(count), first.session->columns());
        meta.insert(QStringLiteral("rows%1").arg(count), first.session->rows());
        return true;
    }

    void tearDown()
    {
        for (const auto &terminal : m_terminals) {
            terminal->view->setSession(nullptr);
            terminal->session->stop();
        }
        m_terminals.clear();
        m_scene.reset();
        m_historyReady = false;
    }

    // Line number at the top of the live screen. The store may have
    // trimmed old history (line or byte limit), so offsets count back from
    // here rather than from the first line ever printed.
    int liveTopLine(const Terminal &terminal) const { return terminal.lineCount + 1 - terminal.session->rows(); }

    // Fills the single terminal's history once; scroll scenarios share it.
    bool ensureHistory()
    {
        if (!setUp(1)) return false;
        if (m_historyReady) return true;
        Terminal &terminal = *m_terminals.front();
        ScriptedProcess *process = terminal.process.load(std::memory_order_acquire);
        QElapsedTimer clock;
        clock.start();
        QByteArray chunk;
        for (int number = 0; number < m_options.history; ++number) {
            chunk += historyLine(number);
            if ((number + 1) % 1000 == 0 || number + 1 == m_options.history) {
                process->emitOutput(chunk);
                chunk.clear();
                waitUntil([] { return false; }, 0);
            }
        }
        terminal.lineCount = m_options.history;
        m_markerLimit = terminal.lineCount;
        m_alternateScreen = false;
        if (!settleAtBottom(120000)) {
            qCritical() << "history fill stalled: view" << terminal.view->scrollbackLines() << "core"
                        << terminal.session->scrollbackLines() << "grid" << terminal.session->columns() << "x"
                        << terminal.session->rows();
            return false;
        }
        meta.insert(QStringLiteral("historyFillMs"), clock.elapsed());
        // What the store kept of the printed lines.
        meta.insert(QStringLiteral("historyHeld"), terminal.view->scrollbackLines());
        m_historyReady = true;
        return true;
    }

    qint64 tickPeriodNs() const { return qint64(1e9 / m_options.hz); }

    // Runs ticks until `done` or the timeout, rendering whenever the scene
    // asks; nothing is recorded.
    bool waitUntil(const std::function<bool()> &done, int timeoutMs)
    {
        QElapsedTimer clock;
        clock.start();
        do {
            tick({}, nullptr);
            if (done()) return true;
        } while (clock.elapsed() < timeoutMs);
        return done();
    }

    // Sleeps until the next tick, then applies `input`, completes resizes
    // and renders if needed. The pause runs the event loop, so queued frames
    // and preparation results arrive between ticks as they would between
    // vsyncs.
    void tick(const std::function<void()> &input, Sample *sample, bool forceRender = false)
    {
        if (!m_tickClock.isValid()) {
            m_tickClock.start();
            m_nextTickNs = 0;
        }
        m_nextTickNs += tickPeriodNs();
        const qint64 now = m_tickClock.nsecsElapsed();
        if (m_nextTickNs < now - 4 * tickPeriodNs()) {
            m_nextTickNs = now; // a long frame does not cause a burst of catch-up ticks
        }
        // Rounded up to whole milliseconds rather than spun: spinning would
        // add its own CPU time to every scenario.
        const qint64 waitMs = (m_nextTickNs - now + 999999) / 1000000;
        if (waitMs > 0) {
            QEventLoop loop;
            QTimer::singleShot(std::chrono::milliseconds(waitMs), Qt::PreciseTimer, &loop, &QEventLoop::quit);
            loop.exec();
        }
        QCoreApplication::processEvents(QEventLoop::AllEvents);
        if (input) input();
        if (!m_scene || (!m_scene->dirty() && !forceRender)) return;
        QVector<int> columnsX;
        for (const auto &terminal : m_terminals) {
            columnsX.push_back(
                int((terminal->view->x() + terminal->view->cellWidth() * 0.5) * m_scene->devicePixelRatio()));
        }
        QVector<QByteArray> columns;
        const FrameTiming timing = m_scene->render(columnsX, &columns);
        if (!sample) return;
        sample->rendered = true;
        sample->timing = timing;
        for (int index = 0; index < int(m_terminals.size()); ++index) {
            const QVector<int> markers = rowMarkers(*m_terminals.at(size_t(index)), columns.at(index));
            int top = -1;
            for (int row = 0; row < markers.size() && top < 0; ++row) {
                if (markers.at(row) >= 0) top = markers.at(row) - (m_alternateScreen ? 0 : row);
            }
            sample->presented[index] = top;
            if (m_alternateScreen) {
                for (const int value : markers) {
                    if (value >= 0 && value != top) sample->torn = true;
                }
            }
        }
    }

    QVector<int> rowMarkers(const Terminal &terminal, const QByteArray &column) const
    {
        QVector<int> markers;
        const qreal dpr = m_scene->devicePixelRatio();
        const int rows = terminal.session->rows();
        const qreal top = terminal.view->y() * dpr;
        for (int row = 0; row < rows; ++row) {
            const int y = int(top + (row + 0.5) * terminal.view->cellHeight() * dpr);
            if (y < 0 || (y + 1) * 4 > column.size()) break;
            const int value = decodeMarker(reinterpret_cast<const uchar *>(column.constData()) + y * 4);
            markers.push_back(value >= 0 && value <= m_markerLimit ? value : -1);
        }
        return markers;
    }

    // Runs `ticks` measured ticks. `input(tick)` drives the terminals and
    // returns the value each picture should show after this tick.
    QVector<Sample> measure(int ticks, const std::function<QVector<int>(int)> &input)
    {
        QVector<Sample> samples;
        samples.reserve(ticks);
        QVector<int> lastPresented(int(m_terminals.size()), -1);
        for (int index = 0; index < ticks; ++index) {
            Sample sample;
            sample.presented = lastPresented;
            tick([&]() { sample.expected = input(index); }, &sample);
            lastPresented = sample.presented;
            samples.push_back(std::move(sample));
        }
        return samples;
    }

    QJsonObject summarize(const QString &name, const QVector<Sample> &samples, double cpuSeconds, qint64 wallMs) const
    {
        QVector<double> polish, sync, render, frame;
        int rendered = 0, overruns = 0, torn = 0;
        const double periodMs = 1000.0 / m_options.hz;
        for (const Sample &sample : samples) {
            if (!sample.rendered) continue;
            ++rendered;
            polish.push_back(sample.timing.polishMs);
            sync.push_back(sample.timing.syncMs);
            render.push_back(sample.timing.renderMs);
            const double total = sample.timing.polishMs + sample.timing.syncMs + sample.timing.renderMs;
            frame.push_back(total);
            if (total > periodMs) ++overruns;
            if (sample.torn) ++torn;
        }
        // Per terminal: how many of the requested changes the picture made
        // (at any later tick), how far it trailed the request, and the
        // longest freeze.
        QVector<double> lag;
        int wanted = 0, followed = 0, longestFreeze = 0;
        for (int terminal = 0; terminal < int(m_terminals.size()); ++terminal) {
            int freeze = 0;
            for (int index = 0; index < samples.size(); ++index) {
                const Sample &sample = samples.at(index);
                if (sample.expected.size() <= terminal) continue;
                const int expected = sample.expected.at(terminal);
                const int presented = sample.presented.at(terminal);
                const int previousExpected = index > 0 && samples.at(index - 1).expected.size() > terminal
                    ? samples.at(index - 1).expected.at(terminal)
                    : expected;
                const int previousPresented = index > 0 ? samples.at(index - 1).presented.at(terminal) : presented;
                if (presented >= 0) lag.push_back(std::abs(expected - presented));
                if (expected != previousExpected) ++wanted;
                if (presented != previousPresented) ++followed;
                freeze = presented == previousPresented && presented != expected ? freeze + 1 : 0;
                longestFreeze = std::max(longestFreeze, freeze);
            }
        }
        int nodes = 0, geometryNodes = 0;
        for (const auto &terminal : m_terminals) {
            countNodes(QQuickItemPrivate::get(terminal->view)->paintNode, &nodes, &geometryNodes);
        }
        const auto round2 = [](double value) { return std::round(value * 100) / 100; };
        QJsonObject result{
            {QStringLiteral("scenario"), name},
            {QStringLiteral("ticks"), samples.size()},
            {QStringLiteral("rendered"), rendered},
            {QStringLiteral("followRatio"), wanted ? round2(std::min(1.0, double(followed) / wanted)) : 1.0},
            // Picture changes per second and terminal: the frame rate a
            // user sees, whatever the window rendered.
            {QStringLiteral("contentFps"),
             round2(wallMs > 0 ? followed * 1000.0 / wallMs / std::max<size_t>(1, m_terminals.size()) : 0)},
            {QStringLiteral("lagMean"),
             round2(lag.isEmpty() ? 0 : std::accumulate(lag.cbegin(), lag.cend(), 0.0) / lag.size())},
            {QStringLiteral("lagP95"), round2(percentile(lag, 0.95))},
            {QStringLiteral("lagMax"), round2(percentile(lag, 1.0))},
            {QStringLiteral("longestFreezeTicks"), longestFreeze},
            {QStringLiteral("polishP50Ms"), round2(percentile(polish, 0.5))},
            {QStringLiteral("polishP95Ms"), round2(percentile(polish, 0.95))},
            {QStringLiteral("syncP50Ms"), round2(percentile(sync, 0.5))},
            {QStringLiteral("syncP95Ms"), round2(percentile(sync, 0.95))},
            {QStringLiteral("syncMaxMs"), round2(percentile(sync, 1.0))},
            {QStringLiteral("renderP50Ms"), round2(percentile(render, 0.5))},
            {QStringLiteral("renderP95Ms"), round2(percentile(render, 0.95))},
            {QStringLiteral("frameP95Ms"), round2(percentile(frame, 0.95))},
            {QStringLiteral("overruns"), overruns},
            {QStringLiteral("tornFrames"), torn},
            {QStringLiteral("nodes"), nodes},
            {QStringLiteral("geometryNodes"), geometryNodes},
            {QStringLiteral("cpuCores"), round2(wallMs > 0 ? cpuSeconds * 1000.0 / wallMs : 0)},
            {QStringLiteral("wallMs"), wallMs},
        };
        return result;
    }

    template <typename Measure> bool timed(const QString &name, QJsonArray *results, Measure body)
    {
        const double cpuStart = processCpuSeconds();
        QElapsedTimer wall;
        wall.start();
        const QVector<Sample> samples = body();
        const double cpu = processCpuSeconds() - cpuStart;
        if (samples.isEmpty()) return false;
        results->append(summarize(name, samples, cpu, wall.elapsed()));
        return true;
    }

    // Returns to the live bottom and waits until the picture shows it.
    bool settleAtBottom(int timeoutMs = 5000)
    {
        Terminal &terminal = *m_terminals.front();
        terminal.view->setScrollOffset(0);
        Sample probe;
        probe.presented = {-1};
        const int expected = liveTopLine(terminal);
        // A picture already at the bottom asks for no frame; probing renders
        // one anyway to read what it shows.
        return waitUntil(
            [&]() {
                tick({}, &probe, true);
                return probe.presented.value(0) == expected;
            },
            timeoutMs);
    }

    // Scrolls up `lines` lines every `everyTicks` ticks from the live bottom.
    bool scroll(const QString &name, int lines, int everyTicks, QJsonArray *results)
    {
        if (!ensureHistory()) return false;
        m_alternateScreen = false;
        m_markerLimit = m_terminals.front()->lineCount;
        if (!settleAtBottom()) qWarning().noquote() << name << ": did not settle at the bottom";
        Terminal &terminal = *m_terminals.front();
        const int history = terminal.view->scrollbackLines();
        const int liveTop = liveTopLine(terminal);
        const int ticks = std::min(m_options.ticks, history / lines * everyTicks);
        return timed(name, results, [&]() {
            return measure(ticks, [&](int index) {
                const int offset = std::min(history, (index / everyTicks + 1) * lines);
                terminal.view->setScrollOffset(offset);
                return QVector<int>{liveTop - offset};
            });
        });
    }

    // Drags the scrollbar across the whole history and back: every tick
    // lands on unseen lines.
    bool scrollbarDrag(const QString &name, QJsonArray *results)
    {
        if (!ensureHistory()) return false;
        m_alternateScreen = false;
        m_markerLimit = m_terminals.front()->lineCount;
        if (!settleAtBottom()) qWarning().noquote() << name << ": did not settle at the bottom";
        Terminal &terminal = *m_terminals.front();
        const int history = terminal.view->scrollbackLines();
        const int liveTop = liveTopLine(terminal);
        const int ticks = m_options.ticks;
        return timed(name, results, [&]() {
            return measure(ticks, [&](int index) {
                const double phase = 2.0 * kPi * (index + 1) / ticks;
                const int offset = int(std::round(history * (0.5 - 0.5 * std::cos(phase))));
                terminal.view->setScrollOffset(offset);
                return QVector<int>{liveTop - offset};
            });
        });
    }

    // The scrollbar drag while the child floods the history: every tick the
    // core parses output and the view asks for unseen lines. Once the history
    // reaches the engine's byte budget, pruning shifts row numbers under the
    // markers and inflates the lag; measure lag with a smaller --history.
    bool scrollbarDragFlood(const QString &name, QJsonArray *results)
    {
        if (!ensureHistory()) return false;
        m_alternateScreen = false;
        if (!settleAtBottom()) qWarning().noquote() << name << ": did not settle at the bottom";
        Terminal &terminal = *m_terminals.front();
        ScriptedProcess *process = terminal.process.load(std::memory_order_acquire);
        const int history = terminal.view->scrollbackLines();
        const int liveTop = liveTopLine(terminal);
        // The marker of history row `r` is `r + shift`.
        const int shift = liveTop - history;
        constexpr int kLinesPerTick = 10;
        const int ticks = m_options.ticks;
        m_markerLimit = terminal.lineCount + ticks * kLinesPerTick;
        const bool measured = timed(name, results, [&]() {
            return measure(ticks, [&](int index) {
                QByteArray bytes;
                for (int line = 0; line < kLinesPerTick; ++line) bytes += historyLine(terminal.lineCount++);
                process->emitOutput(bytes);
                const double phase = 2.0 * kPi * (index + 1) / ticks;
                const int target = liveTop - int(std::round(history * (0.5 - 0.5 * std::cos(phase))));
                // As the scrollbar does: an absolute row, measured from the
                // live top the view knows by now.
                terminal.view->setScrollOffset(terminal.view->scrollbackLines() - (target - shift));
                return QVector<int>{target};
            });
        });
        m_markerLimit = terminal.lineCount;
        return measured;
    }

    // Twenty lines a tick at the live bottom: every row moves each frame.
    bool logFlood(const QString &name, QJsonArray *results)
    {
        if (!ensureHistory()) return false;
        m_alternateScreen = false;
        if (!settleAtBottom()) qWarning().noquote() << name << ": did not settle at the bottom";
        Terminal &terminal = *m_terminals.front();
        ScriptedProcess *process = terminal.process.load(std::memory_order_acquire);
        constexpr int kLinesPerTick = 20;
        m_markerLimit = terminal.lineCount + m_options.ticks * kLinesPerTick;
        const bool measured = timed(name, results, [&]() {
            return measure(m_options.ticks, [&](int) {
                QByteArray bytes;
                for (int line = 0; line < kLinesPerTick; ++line) bytes += historyLine(terminal.lineCount++);
                process->emitOutput(bytes);
                return QVector<int>{liveTopLine(terminal)};
            });
        });
        // Later scrolls start from the new bottom.
        m_markerLimit = terminal.lineCount;
        return measured;
    }

    // One full alternate-screen frame per tick on `count` terminals.
    bool churn(const QString &name, ChurnContent content, int count, QJsonArray *results)
    {
        if (!setUp(count)) return false;
        m_alternateScreen = true;
        m_markerLimit = m_options.ticks + 1;
        for (const auto &terminal : m_terminals) {
            terminal->process.load(std::memory_order_acquire)->emitOutput(QByteArrayLiteral("\x1b[?1049h\x1b[2J"));
        }
        waitUntil([] { return false; }, 200);
        const bool measured = timed(name, results, [&]() {
            return measure(m_options.ticks, [&](int index) {
                QVector<int> expected;
                for (const auto &terminal : m_terminals) {
                    terminal->process.load(std::memory_order_acquire)
                        ->emitOutput(
                            churnFrame(index, terminal->session->columns(), terminal->session->rows(), content));
                    expected.push_back(index);
                }
                return expected;
            });
        });
        for (const auto &terminal : m_terminals) {
            terminal->process.load(std::memory_order_acquire)->emitOutput(QByteArrayLiteral("\x1b[?1049l"));
        }
        waitUntil([] { return false; }, 200);
        return measured;
    }

    Options m_options;
    std::unique_ptr<OffscreenScene> m_scene;
    std::vector<std::unique_ptr<Terminal>> m_terminals;
    QElapsedTimer m_tickClock;
    qint64 m_nextTickNs = 0;
    bool m_historyReady = false;
    bool m_alternateScreen = false;
    int m_markerLimit = 0;
};

void printTable(const QJsonArray &results)
{
    const QStringList columns{QStringLiteral("followRatio"),
                              QStringLiteral("contentFps"),
                              QStringLiteral("lagMean"),
                              QStringLiteral("lagP95"),
                              QStringLiteral("longestFreezeTicks"),
                              QStringLiteral("syncP50Ms"),
                              QStringLiteral("syncP95Ms"),
                              QStringLiteral("syncMaxMs"),
                              QStringLiteral("polishP95Ms"),
                              QStringLiteral("renderP95Ms"),
                              QStringLiteral("frameP95Ms"),
                              QStringLiteral("overruns"),
                              QStringLiteral("tornFrames"),
                              QStringLiteral("nodes"),
                              QStringLiteral("cpuCores")};
    QString header = QStringLiteral("%1").arg(QStringLiteral("scenario"), -16);
    for (const QString &column : columns) header += QStringLiteral(" %1").arg(column, 12);
    qInfo().noquote() << header;
    for (const QJsonValue &value : results) {
        const QJsonObject result = value.toObject();
        QString line = QStringLiteral("%1").arg(result.value(QStringLiteral("scenario")).toString(), -16);
        for (const QString &column : columns) {
            line += QStringLiteral(" %1").arg(result.value(column).toVariant().toString(), 12);
        }
        qInfo().noquote() << line;
    }
}

} // namespace

int main(int argc, char **argv)
{
    // Never on the maintainer's desktop (docs/workflow/testing.md §4).
    if (qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM")) qputenv("QT_QPA_PLATFORM", "offscreen");
    if (qEnvironmentVariableIsEmpty("QT_QPA_FONTDIR")) {
        qputenv("QT_QPA_FONTDIR", (qgetenv("SystemRoot") + "\\Fonts"));
    }
    for (int index = 1; index + 1 < argc; ++index) {
        if (qstrcmp(argv[index], "--scale") == 0) qputenv("QT_SCALE_FACTOR", argv[index + 1]);
    }
    // Qt would keep its pipeline cache in %LOCALAPPDATA%, outside the checkout.
    qputenv("QSG_RHI_DISABLE_DISK_CACHE", "1");
    // The offscreen platform reports no OpenGL, so Qt Quick would fall back
    // to its software adaptation, which paints terminal rows on the CPU.
    QQuickWindow::setSceneGraphBackend(QStringLiteral("rhi"));
    QQuickWindow::setGraphicsApi(QSGRendererInterface::Direct3D11);
    QGuiApplication app(argc, argv);
    TerminalViewportTrace::setEnabled(false);

    QCommandLineParser parser;
    parser.addHelpOption();
    const QCommandLineOption hzOption(QStringLiteral("hz"), QStringLiteral("Tick rate."), QStringLiteral("hz"),
                                      QStringLiteral("144"));
    const QCommandLineOption scaleOption(QStringLiteral("scale"), QStringLiteral("Device pixel ratio."),
                                         QStringLiteral("factor"), QStringLiteral("1"));
    const QCommandLineOption sizeOption(QStringLiteral("size"), QStringLiteral("Window size, WxH."),
                                        QStringLiteral("size"), QStringLiteral("1600x960"));
    const QCommandLineOption historyOption(QStringLiteral("history"), QStringLiteral("History lines."),
                                           QStringLiteral("lines"), QStringLiteral("90000"));
    const QCommandLineOption ticksOption(QStringLiteral("ticks"), QStringLiteral("Measured ticks per scenario."),
                                         QStringLiteral("ticks"), QStringLiteral("480"));
    const QCommandLineOption fontSizeOption(QStringLiteral("font-size"), QStringLiteral("Font pixel size."),
                                            QStringLiteral("pixels"), QStringLiteral("13"));
    const QCommandLineOption scenarioOption(QStringLiteral("scenario"), QStringLiteral("Run only this scenario."),
                                            QStringLiteral("name"));
    const QCommandLineOption jsonOption(QStringLiteral("json"), QStringLiteral("Also write results here."),
                                        QStringLiteral("file"));
    const QCommandLineOption listOption(QStringLiteral("list"), QStringLiteral("List scenarios."));
    parser.addOptions({hzOption, scaleOption, sizeOption, historyOption, ticksOption, fontSizeOption, scenarioOption,
                       jsonOption, listOption});
    parser.process(app);
    if (parser.isSet(listOption)) {
        for (const QString &name : Benchmark::scenarios()) qInfo().noquote() << name;
        return 0;
    }

    // The bundled faces, embedded under the same resource prefix as in the product.
    for (const char *face : {"SpanMono-Regular.ttf", "SpanMono-Bold.ttf", "SpanMono-Italic.ttf",
                             "SpanMono-BoldItalic.ttf", "SpanSymbols-Regular.ttf"}) {
        const QString path = QStringLiteral(":/fonts/") + QLatin1String(face);
        if (QFontDatabase::addApplicationFont(path) < 0) {
            qCritical().noquote() << "cannot register" << path;
            return 1;
        }
    }

    Benchmark::Options options;
    options.hz = std::clamp(parser.value(hzOption).toInt(), 30, 360);
    const QStringList size = parser.value(sizeOption).split(QLatin1Char('x'));
    if (size.size() == 2) options.size = QSize(size.at(0).toInt(), size.at(1).toInt());
    options.history = std::clamp(parser.value(historyOption).toInt(), 1000, kDefaultMaxScrollbackLines - 5000);
    options.ticks = std::max(30, parser.value(ticksOption).toInt());
    options.font = QFont(QStringLiteral("Span Mono"));
    options.font.setPixelSize(std::max(6, parser.value(fontSizeOption).toInt()));

    Benchmark benchmark(options);
    benchmark.meta = {
        {QStringLiteral("hz"), options.hz},
        {QStringLiteral("size"), parser.value(sizeOption)},
        {QStringLiteral("history"), options.history},
        {QStringLiteral("ticks"), options.ticks},
        {QStringLiteral("fontPixelSize"), options.font.pixelSize()},
        {QStringLiteral("qt"), QString::fromLatin1(qVersion())},
        {QStringLiteral("platform"), QGuiApplication::platformName()},
#ifdef QT_DEBUG
        {QStringLiteral("build"), QStringLiteral("debug")},
#else
        {QStringLiteral("build"), QStringLiteral("release")},
#endif
    };

    const QStringList selected = parser.isSet(scenarioOption) ? parser.values(scenarioOption) : Benchmark::scenarios();
    QJsonArray results;
    for (const QString &name : selected) {
        if (!benchmark.runScenario(name, &results)) {
            qCritical().noquote() << "scenario failed:" << name;
            return 1;
        }
    }

    qInfo().noquote() << QJsonDocument(benchmark.meta).toJson(QJsonDocument::Compact);
    printTable(results);
    if (parser.isSet(jsonOption)) {
        QFile file(parser.value(jsonOption));
        if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            qCritical().noquote() << "cannot write" << file.fileName();
            return 1;
        }
        file.write(
            QJsonDocument(QJsonObject{{QStringLiteral("meta"), benchmark.meta}, {QStringLiteral("scenarios"), results}})
                .toJson());
    }
    return 0;
}
