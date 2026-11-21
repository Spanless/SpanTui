#include "terminal/api/terminal_session_input.h"

#include "terminal/core/terminal_core_worker.h"

#include <QCoreApplication>

#include <utility>

#include <algorithm>

namespace span::terminal {

namespace {
constexpr int kPasteUtf16UnitsPerSlice = 4096;
constexpr qsizetype kPasteQueueResumeBytes = 768 * 1024;
constexpr int kMaximumPendingPasteUtf16Units = 16 * 1024 * 1024;
constexpr qsizetype kMaximumPendingEngineWriteBytes = 1024 * 1024;
// Retry delays while the process is starting or backed up.
constexpr int kEngineWriteRetryMs = 16;
constexpr int kPasteRetryMs = 5;
}

TerminalSessionInput::TerminalSessionInput(Sink sink, QObject *parent)
    : QObject(parent),
      m_sink(std::move(sink))
{
    m_pasteTimer.setSingleShot(true);
    connect(&m_pasteTimer, &QTimer::timeout, this, &TerminalSessionInput::drainPaste);
    m_engineWriteTimer.setSingleShot(true);
    connect(&m_engineWriteTimer, &QTimer::timeout, this,
            &TerminalSessionInput::drainEngineWrite);
}

void TerminalSessionInput::cancelQueued(bool includeProtocol)
{
    m_pasteTimer.stop();
    m_engineWriteTimer.stop();
    if (m_pasteOpen) {
        // Close the engine-side bracketed-paste context before dropping the
        // local state, otherwise the engine keeps interpreting every later
        // keystroke as paste content and beginPaste() is silently ignored.
        m_sink.postCore([](TerminalCoreWorker *core) { core->endPaste(); },
                        TerminalInputKind::PasteEnd);
    }
    m_pastes.clear();
    m_pendingPasteUnits = 0;
    m_pasteOffset = 0;
    m_pasteOpen = false;
    m_pendingEngineWrites.erase(
        std::remove_if(m_pendingEngineWrites.begin(), m_pendingEngineWrites.end(),
            [includeProtocol](const PendingWrite &write) {
                return includeProtocol || write.token.kind != TerminalInputKind::Protocol;
            }),
        m_pendingEngineWrites.end());
    m_pendingEngineWriteBytes = 0;
    for (const auto &write : m_pendingEngineWrites)
        m_pendingEngineWriteBytes += write.bytes.size();
    if (!m_pendingEngineWrites.empty()) m_engineWriteTimer.start(0);
}

void TerminalSessionInput::paste(const QString &text, const TerminalPasteOffer &offer)
{
    if (m_pendingPasteUnits >= kMaximumPendingPasteUtf16Units) {
        m_sink.reportError(QCoreApplication::translate(
            "span::terminal::TerminalSession",
            "Paste buffer is full; remaining clipboard text was dropped."));
        return;
    }
    QString chunk = text;
    if (m_pendingPasteUnits + chunk.size() > kMaximumPendingPasteUtf16Units) {
        chunk.truncate(kMaximumPendingPasteUtf16Units - m_pendingPasteUnits);
        m_sink.reportError(QCoreApplication::translate(
            "span::terminal::TerminalSession",
            "Paste truncated to %1 characters.").arg(kMaximumPendingPasteUtf16Units));
    }
    if (chunk.isEmpty() && offer.representations.isEmpty()) {
        return;
    }
    m_pendingPasteUnits += chunk.size();
    m_pastes.push_back({std::move(chunk), offer});
    if (!m_pasteTimer.isActive()) {
        drainPaste();
    }
}

void TerminalSessionInput::writeFromEngine(const QByteArray &bytes,
                                          const TerminalInputToken &token)
{
    if (!token.valid() || bytes.isEmpty() || (!m_sink.running() && !m_sink.processStarting())) return;
    if (m_pendingEngineWrites.empty() && m_sink.writeToProcess(bytes, token.kind)) return;
    if (m_pendingEngineWriteBytes + bytes.size() > kMaximumPendingEngineWriteBytes) {
        m_sink.reportError(QCoreApplication::translate(
            "span::terminal::TerminalSession",
            "Terminal input backlog is full; input was dropped."));
        return;
    }
    m_pendingEngineWrites.push_back({bytes, token});
    m_pendingEngineWriteBytes += bytes.size();
    if (!m_engineWriteTimer.isActive()) m_engineWriteTimer.start(kEngineWriteRetryMs);
}

void TerminalSessionInput::drainEngineWrite()
{
    if (!m_sink.running() && !m_sink.processStarting()) {
        cancelQueued();
        return;
    }
    while (!m_pendingEngineWrites.empty()) {
        const auto &write = m_pendingEngineWrites.front();
        if (write.token.valid()) {
            if (m_sink.pendingProcessBytes() > kPasteQueueResumeBytes
                || !m_sink.writeToProcess(write.bytes, write.token.kind)) {
                m_engineWriteTimer.start(kEngineWriteRetryMs);
                return;
            }
        }
        m_pendingEngineWriteBytes -= write.bytes.size();
        m_pendingEngineWrites.pop_front();
    }
}

void TerminalSessionInput::drainPaste()
{
    if (!m_sink.running() || m_pastes.empty()) {
        return;
    }
    if (m_sink.pendingProcessBytes() > kPasteQueueResumeBytes) {
        m_pasteTimer.start(kPasteRetryMs);
        return;
    }
    const PendingPaste &paste = m_pastes.front();
    if (!m_pasteOpen) {
        m_sink.postCore([offer = paste.offer, hasText = !paste.text.isEmpty()](TerminalCoreWorker *core) {
            core->beginPaste(offer, hasText);
        }, TerminalInputKind::PasteBegin);
        m_pasteOpen = true;
    }
    const int remaining = int(paste.text.size()) - m_pasteOffset;
    if (remaining > 0) {
        int count = std::min(kPasteUtf16UnitsPerSlice, remaining);
        if (count < remaining && paste.text.at(m_pasteOffset + count - 1).isHighSurrogate() &&
            paste.text.at(m_pasteOffset + count).isLowSurrogate()) {
            --count;
        }
        const QString chunk = paste.text.mid(m_pasteOffset, count);
        m_sink.postCore([chunk](TerminalCoreWorker *core) { core->sendPasteBody(chunk); },
                        TerminalInputKind::User);
        m_pasteOffset += count;
    }
    // The last slice closes the bracket in the same turn, so a paste that
    // fits one slice is complete before the next key arrives.
    if (m_pasteOffset >= paste.text.size()) {
        m_sink.postCore([](TerminalCoreWorker *core) { core->endPaste(); },
                        TerminalInputKind::PasteEnd);
        m_pendingPasteUnits -= paste.text.size();
        m_pastes.pop_front();
        m_pasteOffset = 0;
        m_pasteOpen = false;
    }
    if (!m_pastes.empty()) {
        m_pasteTimer.start(0);
    }
}

} // namespace span::terminal
