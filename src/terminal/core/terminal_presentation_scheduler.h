#pragma once

#include <QtGlobal>

namespace span::terminal {

// Decides when the Core may turn the live parser state into a published
// snapshot. It is the single owner of every presentation rule; the Core only
// reports events and arms one timer for the returned recheck time.
//
// A snapshot is a consistent cut only at a cut point: the end of one producer
// write (the PTY pipe had no further bytes when that chunk was read) or the
// end of a DEC 2026 transaction. Between cut points the parser holds part of
// a frame, so output-driven publication waits for the next cut point, bounded
// by kMidWriteHoldMs for a producer that never pauses.
//
// Pure logic over caller-supplied millisecond timestamps; no timers, threads
// or Qt objects, so every rule is testable with a virtual clock.
class TerminalPresentationScheduler final {
public:
    enum class Release : quint8 { None, SynchronizedStall, ImplicitRedraw };

    struct Decision {
        bool publish = false;
        // Absolute time to evaluate again; negative when only a new event can
        // change the decision.
        qint64 recheckAtMs = -1;
        // Publishing now cuts a producer write in the middle.
        bool forcedCut = false;
        // A deferral that ended during this evaluation.
        Release released = Release::None;
    };

    void setFrameIntervalMs(int intervalMs);

    // Live state differs from the last published snapshot.
    void markDirty(qint64 nowMs);
    bool dirty() const { return m_dirty; }
    // Publish at the next evaluation even off the frame cadence (scroll,
    // grid change, consumer attach, transport end); still never inside a
    // 2026 transaction or an implicit redraw.
    void requestImmediate() { m_immediate = true; }

    // One chunk of PTY output has been fed to the parser; `endsWrite` when it
    // reached the end of a producer write.
    void outputConsumed(qint64 nowMs, bool endsWrite);
    void synchronizedBegin(qint64 nowMs);
    void synchronizedEnd(qint64 nowMs);
    void redrawBegin(qint64 nowMs);
    void repaintExpected(qint64 nowMs);
    // Transport ended or was replaced: no protocol transaction can complete.
    void resetProtocol();

    // `outputPending`: bytes are queued but not yet consumed.
    Decision evaluate(qint64 nowMs, bool outputPending);
    void published(qint64 nowMs);

    bool deferred() const;
    bool synchronizedActive() const { return m_sync == Sync::Explicit; }

private:
    enum class Sync : quint8 {
        Inactive,
        Explicit,
        // Watchdog released a stalled 2026; its late reset is still expected.
        ForcedReleased,
        Implicit,
    };

    bool repaintSettling(qint64 nowMs);

    int m_frameIntervalMs = 16;
    bool m_dirty = false;
    bool m_immediate = false;
    bool m_atCutPoint = true;
    Sync m_sync = Sync::Inactive;
    qint64 m_dirtySinceMs = -1;
    qint64 m_lastPublishMs = -1;
    qint64 m_lastOutputMs = -1;
    qint64 m_syncActivityMs = -1;
    qint64 m_redrawStartMs = -1;
    qint64 m_settleUntilMs = -1;
    qint64 m_settleHoldStartMs = -1;
};

} // namespace span::terminal
