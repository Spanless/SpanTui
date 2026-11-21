#include "terminal/api/terminal_session_input.h"
#include "terminal/core/terminal_core_worker.h"
#include "terminal/shared/terminal_frame_store.h"
#include "terminal/shared/terminal_output_queue.h"

#include <QTest>

using namespace span::terminal;

namespace {

TerminalPasteOffer textOffer(const QByteArray &text)
{
    return {{{QByteArrayLiteral("text/plain"), text}}, {}};
}

// The session's input queue in front of a real core and terminal engine; the
// core runs each posted operation at once, in posting order, and `written`
// collects what the child would read.
class Harness final {
public:
    Harness()
        : m_worker(QStringLiteral("input"), 40, 5, 100, m_queue, std::make_shared<TerminalFrameStore>()),
          input(sink())
    {
        m_queue->open();
        m_worker.setInputEpoch(m_epoch);
        m_worker.initialize();
        QObject::connect(&m_worker, &TerminalCoreWorker::outputRequested,
                         [this](const QByteArray &bytes, const TerminalInputToken &) { written += bytes; });
        QObject::connect(&m_worker, &TerminalCoreWorker::pasteDeclined,
                         [this](const QString &message) { errors.append(message); });
    }

    // Output of the child, such as the modes it enables.
    void childWrites(const QByteArray &bytes)
    {
        m_queue->append(bytes);
        m_worker.drainOutput();
    }

    void type(const QString &text)
    {
        post([text](TerminalCoreWorker *core) { core->sendText(text); }, TerminalInputKind::User);
    }

    QByteArray written;
    QStringList errors;

private:
    void post(const std::function<void(TerminalCoreWorker *)> &operation, TerminalInputKind kind)
    {
        m_worker.executeInput({m_epoch, m_epoch->userSerial.load(), kind}, operation);
    }

    TerminalSessionInput::Sink sink()
    {
        TerminalSessionInput::Sink sink;
        sink.postCore = [this](const std::function<void(TerminalCoreWorker *)> &operation, TerminalInputKind kind) {
            post(operation, kind);
        };
        sink.writeToProcess = [](const QByteArray &, TerminalInputKind) { return true; };
        sink.pendingProcessBytes = []() { return qsizetype(0); };
        sink.running = []() { return true; };
        sink.processStarting = []() { return false; };
        sink.reportError = [this](const QString &message) { errors.append(message); };
        return sink;
    }

    std::shared_ptr<TerminalOutputQueue> m_queue = std::make_shared<TerminalOutputQueue>();
    std::shared_ptr<TerminalInputEpoch> m_epoch = std::make_shared<TerminalInputEpoch>();
    TerminalCoreWorker m_worker;

public:
    TerminalSessionInput input;
};

} // namespace

class TerminalSessionInputTest final : public QObject {
    Q_OBJECT

private slots:
    void consecutivePastesEachArriveInTheirOwnBracket();
    void keyTypedAfterAPasteReachesTheChildAfterIt();
    void childThatTakesPasteEventsGetsAnEventForEveryPaste();
    void declinedTextlessOfferReportsItsMessage();
};

void TerminalSessionInputTest::consecutivePastesEachArriveInTheirOwnBracket()
{
    Harness harness;
    harness.childWrites("\x1b[?2004h");

    harness.input.paste(QStringLiteral("one"), textOffer("one"));
    harness.input.paste(QStringLiteral("two"), textOffer("two"));

    QCOMPARE(harness.written, QByteArray("\x1b[200~one\x1b[201~\x1b[200~two\x1b[201~"));
}

void TerminalSessionInputTest::keyTypedAfterAPasteReachesTheChildAfterIt()
{
    Harness harness;
    harness.childWrites("\x1b[?2004h");

    harness.input.paste(QStringLiteral("ls"), textOffer("ls"));
    harness.type(QStringLiteral("x"));

    QCOMPARE(harness.written, QByteArray("\x1b[200~ls\x1b[201~x"));
}

void TerminalSessionInputTest::childThatTakesPasteEventsGetsAnEventForEveryPaste()
{
    Harness harness;
    harness.childWrites("\x1b[?2004h\x1b[?5522h");

    harness.input.paste(QStringLiteral("first secret"), textOffer("first secret"));
    harness.input.paste(QStringLiteral("second secret"), textOffer("second secret"));

    // An event is an OK, DATA (its MIME types) and DONE sequence.
    QCOMPARE(harness.written.count("\x1b]5522;type=read:status=DONE:"), 2);
    QVERIFY2(!harness.written.contains("secret"), harness.written.constData());
    QVERIFY2(!harness.written.contains("\x1b[200~"), harness.written.constData());
}

void TerminalSessionInputTest::declinedTextlessOfferReportsItsMessage()
{
    Harness harness;

    harness.input.paste(
        QString(), {{{QByteArrayLiteral("image/png"), QByteArrayLiteral("PNG")}}, QStringLiteral("no image here")});

    QCOMPARE(harness.written, QByteArray());
    QCOMPARE(harness.errors, QStringList{QStringLiteral("no image here")});
}

QTEST_MAIN(TerminalSessionInputTest)

#include "terminal_session_input_test.moc"
