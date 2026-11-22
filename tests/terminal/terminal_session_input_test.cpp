#include "terminal/api/terminal_session_input.h"
#include "terminal/core/terminal_core_worker.h"
#include "terminal/shared/terminal_frame_store.h"
#include "terminal/shared/terminal_output_queue.h"

#include <QRegularExpression>
#include <QTest>

#include <utility>

using namespace span::terminal;

namespace {

TerminalPasteOffer textOffer(const QByteArray &text)
{
    return {{{QByteArrayLiteral("text/plain"), text}}, {}};
}

// Mirrors the process backends' budget for bytes the child has not read.
constexpr qsizetype kBackendBudgetBytes = 1024 * 1024;

// The session's input queue in front of a real core and terminal engine; the
// core runs each posted operation at once, in posting order, and the engine's
// output goes back through the queue to a backend with a bounded budget.
// `written` collects what the child read; it reads everything at once unless
// a test stalls it.
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
                         [this](const QByteArray &bytes, const TerminalInputToken &token) {
                             input.writeFromEngine(bytes, token);
                         });
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

    // The child stops reading; the backend holds what is written until its
    // budget is spent and then refuses.
    void stallChild() { m_childReading = false; }

    // The child reads what the backend holds.
    void childReads() { written += std::exchange(m_unread, {}); }

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
        sink.writeToProcess = [this](const QByteArray &bytes, TerminalInputKind) {
            if (m_unread.size() + bytes.size() > kBackendBudgetBytes) return false;
            m_unread += bytes;
            if (m_childReading) childReads();
            return true;
        };
        sink.pendingProcessBytes = [this]() { return m_unread.size(); };
        sink.running = []() { return true; };
        sink.processStarting = []() { return false; };
        sink.reportError = [this](const QString &message) { errors.append(message); };
        return sink;
    }

    QByteArray m_unread;
    bool m_childReading = true;
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
    void childThatIsSlowToReadGetsAWholePastedImage();
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

void TerminalSessionInputTest::childThatIsSlowToReadGetsAWholePastedImage()
{
    Harness harness;
    harness.childWrites("\x1b[?5522h");
    // An image whose encoded answer is several times the backend's budget.
    QByteArray image(3 * 1024 * 1024, Qt::Uninitialized);
    for (qsizetype index = 0; index < image.size(); ++index) image[index] = char(index * 131 % 251);
    harness.input.paste(QString(), {{{QByteArrayLiteral("image/png"), image}}, {}});
    const QRegularExpressionMatch password = QRegularExpression(QStringLiteral("pw=([^:;\\x07\\x1b]+)"))
                                                 .match(QString::fromLatin1(std::exchange(harness.written, {})));
    QVERIFY(password.hasMatch());

    // The child asks for the image and reads its input slower than the
    // answer arrives.
    harness.stallChild();
    harness.childWrites("\x1b]5522;type=read:pw=" + password.captured(1).toLatin1() + ":name="
                        + QByteArrayLiteral("test").toBase64() + ";" + QByteArrayLiteral("image/png").toBase64()
                        + "\x1b\\");
    QTRY_VERIFY_WITH_TIMEOUT((harness.childReads(), harness.written.contains("status=DONE")), 10000);

    QCOMPARE(harness.errors, QStringList());
    QByteArray received;
    const QRegularExpression chunk(
        QStringLiteral("\\x1b\\]5522;type=read:status=DATA[^;]*;([A-Za-z0-9+/=]*)(?:\\x1b\\\\|\\x07)"));
    for (const QRegularExpressionMatch &match : chunk.globalMatch(QString::fromLatin1(harness.written)))
        received += QByteArray::fromBase64(match.captured(1).toLatin1());
    QCOMPARE(received.size(), image.size());
    QVERIFY(received == image);
}

QTEST_MAIN(TerminalSessionInputTest)

#include "terminal_session_input_test.moc"
