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
    void paste(const QString &text);
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
    QString m_pendingPaste;
    int m_pendingPasteOffset = 0;
    bool m_pasteOpen = false;
};

} // namespace span::terminal
