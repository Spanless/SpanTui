#include "terminal/core/terminal_presentation_scheduler.h"

#include "terminal/terminal_constants.h"

#include <algorithm>

namespace span::terminal {

void TerminalPresentationScheduler::setFrameIntervalMs(int intervalMs) {
    m_frameIntervalMs = std::max(1, intervalMs);
}

void TerminalPresentationScheduler::markDirty(qint64 nowMs) {
    if (m_dirty) return;
    m_dirty = true;
    m_dirtySinceMs = nowMs;
}

void TerminalPresentationScheduler::outputConsumed(qint64 nowMs, bool endsWrite) {
    m_lastOutputMs = nowMs;
    m_atCutPoint = endsWrite;
    // A large 2026 frame still arriving through a slow PTY is not stalled.
    if (m_sync == Sync::Explicit) m_syncActivityMs = nowMs;
}

void TerminalPresentationScheduler::synchronizedBegin(qint64 nowMs) {
    // DECSET is a state setter, not an opening delimiter: repeating it keeps
    // the transaction. A new set supersedes a forced release whose reset was
    // lost, and an implicit redraw hands over to the explicit transaction.
    if (m_sync == Sync::Explicit) return;
    m_sync = Sync::Explicit;
    m_syncActivityMs = nowMs;
}

void TerminalPresentationScheduler::synchronizedEnd(qint64 nowMs) {
    // A stray reset (inbox ConPTY emits an empty 2026 pair) does not end the
    // reprint that ED3 announced.
    if (m_sync == Sync::Implicit) return;
    if (m_sync != Sync::Explicit) {
        // DECRST is idempotent, including the late reset after a watchdog
        // release.
        m_sync = Sync::Inactive;
        return;
    }
    m_sync = Sync::Inactive;
    // The application declared this state complete: a cut point regardless
    // of where the producer's write ends.
    m_atCutPoint = true;
    markDirty(nowMs);
}

void TerminalPresentationScheduler::redrawBegin(qint64 nowMs) {
    // An explicit 2026 block is already atomic. A second ED3 during an
    // implicit redraw keeps the original cap, so continuous reprints (a long
    // window drag) still present periodically.
    if (m_sync == Sync::Explicit) return;
    if (m_sync != Sync::Implicit) {
        m_sync = Sync::Implicit;
        m_redrawStartMs = nowMs;
    }
    m_lastOutputMs = nowMs;
}

void TerminalPresentationScheduler::repaintExpected(qint64 nowMs) {
    m_settleUntilMs = nowMs + kRepaintSettleWindowMs;
    m_settleHoldStartMs = -1;
}

void TerminalPresentationScheduler::resetProtocol() {
    m_sync = Sync::Inactive;
    m_atCutPoint = true;
    m_settleUntilMs = -1;
    m_settleHoldStartMs = -1;
}

bool TerminalPresentationScheduler::deferred() const {
    return m_sync == Sync::Explicit || m_sync == Sync::Implicit;
}

bool TerminalPresentationScheduler::repaintSettling(qint64 nowMs) {
    if (m_settleUntilMs < 0) return false;
    if (nowMs < m_settleUntilMs) return true;
    m_settleUntilMs = -1;
    return false;
}

TerminalPresentationScheduler::Decision
TerminalPresentationScheduler::evaluate(qint64 nowMs, bool outputPending) {
    Decision decision;
    if (m_sync == Sync::Explicit) {
        const qint64 stallAt = m_syncActivityMs + kSynchronizedStallMs;
        if (nowMs < stallAt) {
            decision.recheckAtMs = stallAt;
            return decision;
        }
        // The application did not complete its declared transaction. Release
        // the latest parser state on the normal cadence.
        m_sync = Sync::ForcedReleased;
        decision.released = Release::SynchronizedStall;
        markDirty(nowMs);
    } else if (m_sync == Sync::Implicit) {
        const qint64 capAt = m_redrawStartMs + kImplicitRedrawMaxHoldMs;
        const qint64 quietAt = m_lastOutputMs + kImplicitRedrawQuietMs;
        // Queued bytes are the rest of the reprint; consuming them moves the
        // quiet edge.
        if (nowMs < capAt && (outputPending || nowMs < quietAt)) {
            decision.recheckAtMs =
                std::min(capAt, quietAt > nowMs ? quietAt : nowMs + kImplicitRedrawQuietMs);
            return decision;
        }
        // The finished reprint is one frame, not another quiet period.
        m_sync = Sync::Inactive;
        decision.released = Release::ImplicitRedraw;
        markDirty(nowMs);
        m_immediate = true;
    }

    if (!m_dirty) return decision;
    if (m_immediate) {
        decision.publish = true;
        return decision;
    }

    qint64 dueAt = 0;
    if (repaintSettling(nowMs)) {
        // Every chunk of the repaint burst moves the quiet edge; one burst is
        // still presented after kRepaintMaxHoldMs at the latest.
        if (m_settleHoldStartMs < 0) m_settleHoldStartMs = nowMs;
        dueAt = std::min(std::max(m_lastOutputMs, m_dirtySinceMs) + kRepaintQuietMs,
                         m_settleHoldStartMs + kRepaintMaxHoldMs);
    } else {
        dueAt = m_lastPublishMs < 0 ? nowMs : m_lastPublishMs + m_frameIntervalMs;
        if (!m_atCutPoint) {
            // The parser holds part of a producer write. Wait for its end;
            // the next consumed chunk that ends it re-evaluates.
            dueAt = std::max(dueAt, m_dirtySinceMs + kMidWriteHoldMs);
            decision.forcedCut = true;
        }
    }
    if (nowMs >= dueAt) {
        decision.publish = true;
    } else {
        decision.recheckAtMs = dueAt;
        decision.forcedCut = false;
    }
    return decision;
}

void TerminalPresentationScheduler::published(qint64 nowMs) {
    m_lastPublishMs = nowMs;
    m_dirty = false;
    m_dirtySinceMs = -1;
    m_immediate = false;
    m_settleHoldStartMs = -1;
}

} // namespace span::terminal
