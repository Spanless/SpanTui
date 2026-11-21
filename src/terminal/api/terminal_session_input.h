#pragma once

#include "terminal/api/terminal_input_transaction.h"
#include <QByteArray>
#include <QObject>
#include <QString>
#include <QTimer>

#include <functional>
#include <deque>

namespace span::terminal {

class TerminalCoreWorker;

class TerminalSessionInput final : public QObject {
    Q_OBJECT

public:
    struct Sink {
        std::function<void(const std::function<void(TerminalCoreWorker *)> &, TerminalInputKind)> postCore;
        // False until the process has started, or while it is backed up.
        std::function<bool(const QByteArray &, TerminalInputKind)> writeToProcess;
        std::function<qsizetype()> pendingProcessBytes;
        std::function<bool()> running;
        std::function<bool()> processStarting;
        std::function<void(const QString &)> reportError;
    };

    explicit TerminalSessionInput(Sink sink, QObject *parent = nullptr);

    void cancelQueued(bool includeProtocol = true);
    // Queues one paste behind earlier ones, each in its own bracket. Text
    // that fits one slice is posted at once, ahead of anything typed after
    // it; a longer paste continues on a timer as the process takes it.
    void paste(const QString &text, const TerminalPasteOffer &offer = {});
    void writeFromEngine(const QByteArray &bytes, const TerminalInputToken &token);

private slots:
    void drainPaste();
    void drainEngineWrite();

private:
    Sink m_sink;
    QTimer m_pasteTimer;
    QTimer m_engineWriteTimer;
    struct PendingWrite {
        QByteArray bytes;
        TerminalInputToken token;
    };
    std::deque<PendingWrite> m_pendingEngineWrites;
    qsizetype m_pendingEngineWriteBytes = 0;
    struct PendingPaste {
        QString text;
        TerminalPasteOffer offer;
    };
    std::deque<PendingPaste> m_pastes;
    // UTF-16 units of every queued paste, the front one whole.
    qsizetype m_pendingPasteUnits = 0;
    // Sent units of the front paste.
    int m_pasteOffset = 0;
    // The front paste's begin was posted.
    bool m_pasteOpen = false;
};

} // namespace span::terminal
