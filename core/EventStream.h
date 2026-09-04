#ifndef EVENTSTREAM_H__
#define EVENTSTREAM_H__
#include <atomic>
#include <cstdint>
#include <cstring>
#include <thread>
#include <stdexcept>
#include <type_traits>
#include "ETCS_API.h"
#include "LMAXSequentialSharedPage.h"
namespace ETCS {
// GAP_DEPTH is the SLOT count -- events in flight, and the width of
// blocked_by_, which is indexed by slot. Not TAG_BITS (Bundles.h), the TYPE
// vocabulary GapSlot's mask is over. Both were 64, which made them look like
// one number; conflating them silently capped a module at 64 tags, past which
// two ops on the SAME type stopped serializing.
static constexpr size_t GAP_DEPTH = 64;
struct GapSlot
{
    enum class Status : uint8_t { Empty, Pending, Running, Complete, Committed };
    uint64_t sequence         = 0;
    TagMask  tag_closure_mask{};
    Status   status           = Status::Empty;
    uint8_t  _pad[7];
    void*    result           = nullptr;
};
// 8 + (TAG_BITS/8) + 1 + 7 + 8. Was 32 back when the mask was a single
// word. Not padded to a cache line: there is exactly one consumer thread
// touching slots_, so there is no false sharing to buy off, and a denser
// array is better for acquire()'s own full scan.
static_assert(sizeof(GapSlot) == 24 + (TAG_BITS / 8), "GapSlot layout changed");
struct GapReorderBuffer
{
    alignas(64) uint64_t blocked_by_[GAP_DEPTH]{};
    GapSlot slots_[GAP_DEPTH]{};
    // acquire — reserve a slot and record what it must wait for. Leaves it
    // PENDING, a state that now survives: the caller asks blocked() and starts
    // the work only if the answer is no; otherwise the slot sits here holding
    // its event until service() launches it.
    //
    // An ADMISSION barrier, which is the point of the restructure. blocked_by_
    // used to be computed after the handler had run, so it could only delay the
    // EMISSION of a result that already happened -- invisible while every
    // handler was synchronous on one thread, worthless the moment one is not.
    // Same computation, moved ahead of dispatch.
    GapSlot& acquire(uint64_t sequence, const TagMask& tag_closure_mask)
    {
        size_t   idx  = sequence % GAP_DEPTH;
        GapSlot& slot = slots_[idx];
        slot.sequence         = sequence;
        slot.tag_closure_mask = tag_closure_mask;
        slot.status           = GapSlot::Status::Pending;
        slot.result           = nullptr;
        blocked_by_[idx]      = 0;
        for (size_t i = 0; i < GAP_DEPTH; ++i)
        {
            if (i == idx) continue;
            const GapSlot& other = slots_[i];
            if (other.status == GapSlot::Status::Empty ||
                other.status == GapSlot::Status::Committed) continue;
            if (other.tag_closure_mask.intersects(tag_closure_mask))
                blocked_by_[idx] |= (uint64_t(1) << i);
        }
        return slot;
    }
    bool blocked(uint64_t sequence) const
    {
        return blocked_by_[sequence % GAP_DEPTH] != 0;
    }
    // Before the handler, never after: service() sweeps for Pending, so a slot
    // still Pending across its own launch is launched again, forever.
    void mark_running(uint64_t sequence)
    {
        slots_[sequence % GAP_DEPTH].status = GapSlot::Status::Running;
    }
    // abandon — hand a slot back uncommitted, for DispatchKind::Drop, which has
    // no completion to order: its handler released its own caller and nothing
    // downstream reads a result. Clearing the bit from everyone else matters --
    // the slot was live from acquire() to here, so later arrivals may already
    // be recorded as blocked by it.
    void abandon(uint64_t sequence)
    {
        size_t idx = sequence % GAP_DEPTH;
        slots_[idx].status = GapSlot::Status::Empty;
        blocked_by_[idx]   = 0;
        const uint64_t my_bit = uint64_t(1) << idx;
        for (size_t j = 0; j < GAP_DEPTH; ++j) blocked_by_[j] &= ~my_bit;
    }
    bool complete(uint64_t sequence, void* result)
    {
        size_t   idx  = sequence % GAP_DEPTH;
        GapSlot& slot = slots_[idx];
        slot.result = result;
        slot.status = GapSlot::Status::Complete;
        return blocked_by_[idx] == 0;
    }
    // service — the engine. Two jobs that feed each other: committing a slot
    // can unblock a PENDING one (which, if synchronous, completes on the spot
    // and unblocks more) or a COMPLETE one. Hence the fixpoint, where this used
    // to be a single pass -- a missed pass was a latency blip for an emission;
    // for a LAUNCH it is work idling with nothing scheduled to notice.
    //
    // launch() must move the slot out of Pending before returning (see
    // mark_running) or the next sweep re-launches it.
    template<typename LaunchFn, typename EmitFn>
    void service(LaunchFn&& launch, EmitFn&& emit)
    {
        bool progress = true;
        while (progress)
        {
            progress = false;
            for (size_t i = 0; i < GAP_DEPTH; ++i)
            {
                GapSlot& slot = slots_[i];
                if (blocked_by_[i] != 0) continue;
                if (slot.status == GapSlot::Status::Pending)
                {
                    launch(slot);       // -> Running, Complete, or Empty (Drop)
                    progress = true;
                    continue;           // whatever it became, next sweep sees it
                }
                if (slot.status != GapSlot::Status::Complete) continue;
                const uint64_t my_bit = uint64_t(1) << i;
                for (size_t j = 0; j < GAP_DEPTH; ++j)
                    if (j != i) blocked_by_[j] &= ~my_bit;
                slot.status = GapSlot::Status::Committed;
                emit(slot);             // THE commit: this is what releases the
                slot.status = GapSlot::Status::Empty;   // blocked caller
                progress = true;
            }
        }
    }
};
enum class DispatchKind : uint8_t { Inline, Async, Drop };
struct DispatchResult
{
    // No mask here any more -- it moved to Derived::mask_for(state, event),
    // which runs BEFORE dispatch, because a mask returned by the handler cannot
    // gate the handler. Deleted rather than left ignored so every site had to
    // be revisited; a silently dropped mask is this epoch's whole failure mode.
    DispatchKind kind = DispatchKind::Drop;
    // Handed to on_emit as GapSlot::result on commit. For the loader that is
    // the DLInEvent itself, whose completion flag on_emit stores -- releasing
    // the blocked caller IS the commit, so no handler stores it. nullptr when
    // nothing waits (Ack; a forward whose completion belongs to the far side).
    void* completion  = nullptr;
};
struct WorkResult
{
    uint64_t origin_seq = 0;
    // RequestUnload's own payload -- the permanent global Module to
    // re-check when this WorkResult reaches on_completion. nullptr for
    // any other use of WorkResult (none exist yet elsewhere in this
    // codebase -- this is the first thing to actually exercise
    // DispatchKind::Async at all).
    void* async_target = nullptr;
};
// Thrown by EventStream::start() specifically when ordering_thread_ is
// still joinable at the moment a fresh start() is attempted -- see that
// method's own comment. A distinct type (rather than a plain
// std::runtime_error) so RegisterDynamicLoader (ETCS_API.h) can catch this
// SPECIFICALLY and distinguish "OS-level zombie DLL, reloaded too fast"
// from any other failure (e.g. a genuine allocation/OOM failure inside
// allocateTunedRing) -- the two conditions warrant different crash
// messages, and conflating them behind a single std::exception catch
// would lose that distinction.
struct EventStreamZombieException : std::runtime_error
{
    using std::runtime_error::runtime_error;
};
template<typename Derived, typename State, typename InEvent>
struct EventStream
{
protected:
    State                     state_;
    GapReorderBuffer          reorder_;
    // The event each slot holds, parallel to reorder_.slots_. A separate array
    // rather than a GapSlot member so GapSlot stays a fixed 56 bytes
    // independent of InEvent -- its size is asserted, and GapReorderBuffer sits
    // inside EventNode, whose layout loader and modules must agree on.
    //
    // Live only while a slot is Pending: acquire() records here, launch_slot()
    // reads back, possibly many loop iterations later. The event lives in its
    // caller's stack frame, which survives exactly that long -- the caller is
    // spinning on a completion flag this stream has not stored yet.
    InEvent                   parked_[GAP_DEPTH]{};
    LMAXSequentialSharedPage* input_ring_      = nullptr;
    LMAXSequentialSharedPage* completion_ring_ = nullptr;
    LMAXSequentialSharedPage* output_ring_     = nullptr;
    uint64_t                  in_seq_          = 0;
    uint64_t                  tail_seq_        = 0;
    uint64_t                  cmp_seq_         = 0;
    uint64_t                  stall_reports_   = 0;
    /*
 * blocked_admissions_ — how many events arrived and could NOT start, because
 * their mask met a live slot's.
 *
 * The one number that says whether the ordering masks are the right width,
 * and it is not a timing. Throughput answers the question only through the
 * scheduler, which on a loaded machine answers something else entirely (the
 * capability suite measured a 4x swing on a fixed control). This counts the
 * decision itself: same traffic, narrower masks, fewer refusals -- an
 * integer, reproducible to the event.
 *
 * Written only by the ordering thread; atomic for readers, relaxed because
 * nothing is ordered against it.
 */
    std::atomic<uint64_t>     blocked_admissions_{ 0 };
    std::thread               ordering_thread_;
    std::atomic<bool>         stop_{ false };
    // Set true the instant shutdown begins (start of stop()/~EventStream()),
    // BEFORE stop_ is even set and well before the ordering thread is
    // joined. Distinct from stop_: stop_ tells the ordering LOOP to exit;
    // this tells enqueue() to refuse new work immediately, rather than
    // silently accepting it into a ring nothing will ever be left alive to
    // drain. This is what makes the loader's own ack-back-to-the-
    // originating-module mechanism safe to attempt unconditionally: if the
    // target module's own stream is already cleaning up, the ack enqueue
    // fails fast and the loader just moves on, instead of blocking forever
    // waiting for a thread that's already gone (or about to be) to service
    // it -- which is exactly the hang this flag exists to prevent.
    std::atomic<bool>         is_cleaning_up_{ false };
    // Default all(): fail-shut, and it makes the restructure a no-op for any
    // stream that does not override -- every slot blocks on every earlier one,
    // which is what a single ordering thread already did. A Derived declaring
    // its own hides this; refining it is an explicit claim that two things may
    // run at once.
    TagMask mask_for(State&, const InEvent&) { return TagMask::all(); }
    // launch_slot — run one admitted event's handler. Reached from
    // ordering_loop when it arrived unblocked, or from service() when its last
    // blocker committed. Identical either way; admission is the only difference.
    void launch_slot(uint64_t seq)
    {
        reorder_.mark_running(seq);
        DispatchResult dr = static_cast<Derived*>(this)->on_event(
            state_, parked_[seq % GAP_DEPTH], seq);
        switch (dr.kind)
        {
            // Done, so eligible to commit -- subject to its blockers, which
            // is why this does not emit.
            case DispatchKind::Inline: reorder_.complete(seq, dr.completion); break;
            // Handed off. Stays Running, holding dependents back, until the
            // worker posts a WorkResult to completion_ring_.
            case DispatchKind::Async:  break;
            // Nothing to order. See GapReorderBuffer::abandon.
            case DispatchKind::Drop:   reorder_.abandon(seq); break;
        }
    }
    void service()
    {
        reorder_.service(
            [this](GapSlot& s) { launch_slot(s.sequence); },
            [this](GapSlot& s) { static_cast<Derived*>(this)->on_emit(state_, s); });
    }
    void drain_completions()
    {
        while (true)
        {
            const LBuffer* buf = completion_ring_->acquireRead(cmp_seq_);
            if (!buf) break;
            WorkResult* wr = nullptr;
            std::memcpy(&wr, buf->buf, sizeof(WorkResult*));
            completion_ring_->markConsumed(cmp_seq_);
            ++cmp_seq_;
            if (!wr) continue;
            static_cast<Derived*>(this)->on_completion(state_, wr, wr->origin_seq);
            reorder_.complete(wr->origin_seq, wr);
        }
        service();
    }
    void ordering_loop()
    {
        int retry = 0;
        while (!stop_.load(std::memory_order_acquire))
        {
            drain_completions();
            while (tail_seq_ < in_seq_ &&
                   reorder_.slots_[tail_seq_ % GAP_DEPTH].status == GapSlot::Status::Empty)
            {
                ++tail_seq_;
            }
            if (in_seq_ - tail_seq_ >= GAP_DEPTH)
            {
                // Backpressure at saturation is NORMAL -- the window is full
                // because producers outrun one consumer. Only a stall that does
                // not CLEAR is a fault. The old condition keyed off
                // (retry & 0xFFFF) == 0, and retry is 0 on the first iteration
                // of every episode, so it dumped 64 slots per episode --
                // thousands of times at 10M events, until the terminal was the
                // bottleneck. One report on the first stall, then only sustained
                // spins, then silence.
                constexpr int      SUSTAINED   = 1 << 14;
                constexpr uint64_t MAX_REPORTS = 8;
                const bool sustained = retry >= SUSTAINED && (retry & (SUSTAINED - 1)) == 0;
                if (stall_reports_ < MAX_REPORTS && (stall_reports_ == 0 || sustained))
                {
                    size_t pending = 0, running = 0, complete = 0, blocked = 0;
                    for (size_t i = 0; i < GAP_DEPTH; ++i)
                    {
                        switch (reorder_.slots_[i].status)
                        {
                            case GapSlot::Status::Pending:  ++pending;  break;
                            case GapSlot::Status::Running:  ++running;  break;
                            case GapSlot::Status::Complete: ++complete; break;
                            default: break;
                        }
                        if (reorder_.blocked_by_[i]) ++blocked;
                    }
                    // pending>0 is admission blocking (masks too coarse, or a
                    // blocker stuck). running>0 is async work that never posted.
                    // blocked==0 on a full window is plain slot exhaustion --
                    // saturation, not a fault.
                    ++stall_reports_;
                    ETCS_LOG("EventStream", "REORDER STALL in=" << in_seq_
                             << " tail=" << tail_seq_ << " cmp=" << cmp_seq_
                             << " pending=" << pending << " running=" << running
                             << " complete=" << complete << " blocked=" << blocked
                             << (stall_reports_ == MAX_REPORTS
                                 ? " (further stall reports suppressed)" : ""));
                }
                LMAXSequentialSharedPage::progressiveYield(retry);
                continue;
            }
            const LBuffer* buf = input_ring_->acquireRead(in_seq_);
            if (!buf)
            {
                LMAXSequentialSharedPage::progressiveYield(retry);
                continue;
            }
            retry = 0;
            InEvent evt;
            std::memcpy(&evt, buf->buf, sizeof(InEvent));
            input_ring_->markConsumed(in_seq_);
            // Mask FIRST, from the event alone -- the handler has not run and
            // must not have to. Every loader kind answers this way (see
            // LoaderStream::mask_for): the key names the module, the mask rides
            // on the event, or the answer is all().
            const TagMask mask = static_cast<Derived*>(this)->mask_for(state_, evt);
            parked_[in_seq_ % GAP_DEPTH] = evt;
            reorder_.acquire(in_seq_, mask);
            // Admitted now, or parked for service() to start once its blockers
            // commit. Under an all() mask with synchronous handlers the parked
            // branch is never taken -- each slot empties before the next
            // arrives, which is why this restructure changes nothing yet.
            if (!reorder_.blocked(in_seq_))
                launch_slot(in_seq_);
            else
                blocked_admissions_.fetch_add(1, std::memory_order_relaxed);
            service();
            ++in_seq_;
        }
        ETCS_LOG("EventStream", "Ordering thread exiting. in_seq="
            << in_seq_ << " cmp_seq=" << cmp_seq_ << " blocked_admissions="
            << blocked_admissions_.load(std::memory_order_relaxed));
    }
public:
    // See blocked_admissions_. Ratio against in_seq_ is the useful reading:
    // what fraction of arrivals this stream's masks refused to let start.
    uint64_t blockedAdmissions() const
    { return blocked_admissions_.load(std::memory_order_relaxed); }
    EventStream()  = default;
    ~EventStream()
    {
        is_cleaning_up_.store(true, std::memory_order_release);
        ETCS_LOG("EventStream", "dtor called, joining ordering thread...");
        if (ordering_thread_.joinable())
        {
            stop_.store(true, std::memory_order_release);
            ordering_thread_.join();
        }
        ETCS_LOG("EventStream", "dtor called, ordering thread joined.");
    }
    EventStream(const EventStream&)            = delete;
    EventStream& operator=(const EventStream&) = delete;
    void start(MemoryArena& arena, int producer_count = 16)
    {
        // A still-joinable ordering_thread_ here means this DSO instance
        // is still entangled with a PRIOR instance's own OS-level
        // teardown that hasn't actually finished yet (dlclose() returning
        // control to the caller is not the same guarantee as glibc having
        // finished unmapping/cleaning up that mapping -- see the session
        // notes on this). That's not a condition safe to quietly join
        // through and proceed past: SandboxGuard's own "Initialized
        // sandbox registry..." log line failing to appear on exactly this
        // same kind of load (confirmed independently, a SEPARATE magic
        // static exhibiting the identical "already looks constructed but
        // isn't genuinely ready" symptom) is direct evidence this isn't
        // an EventStream-local problem -- the whole DSO instance may be
        // sharing stale state with its predecessor. Proceeding anyway is
        // exactly the "weird zombie DLL" that later surfaces as
        // something else entirely (a segfault mid-dispatch, in this
        // session's case), somewhere downstream of here, disconnected
        // from the actual cause. Fail loudly and immediately instead --
        // RegisterDynamicLoader's own try/catch (ETCS_API.h) already
        // catches this, logs it, and returns nullptr, which
        // Module::registerLoader already correctly treats as a failed
        // load. Whether a caller should retry after a short delay is a
        // separate, later decision -- this is just refusing to silently
        // paper over the condition.
        if (ordering_thread_.joinable())
        {
            throw EventStreamZombieException(
                "EventStream::start() called while ordering_thread_ is "
                "still joinable -- this DSO instance is not yet fully "
                "detached from a prior instance's own OS-level teardown "
                "(dlclose() returning does not guarantee glibc has "
                "finished unmapping/cleaning up the previous mapping). "
                "Refusing to proceed rather than load against "
                "potentially stale shared state.");
        }
        input_ring_      = allocateTunedRing(arena, 0, producer_count);
        completion_ring_ = allocateTunedRing(arena, 1, producer_count);
        output_ring_     = allocateTunedRing(arena, 2, 1);
        stop_.store(false, std::memory_order_relaxed);
        is_cleaning_up_.store(false, std::memory_order_relaxed);
        ordering_thread_ = std::thread([this]() { ordering_loop(); });
        ETCS_LOG("EventStream", "Ordering thread started.");
    }
    void stop()
    {
        is_cleaning_up_.store(true, std::memory_order_release);
        stop_.store(true, std::memory_order_release);
        if (ordering_thread_.joinable())
            ordering_thread_.join();
        ETCS_LOG("EventStream", "Ordering thread stopped.");
    }
    // Returns false if the enqueue was refused (this stream is cleaning
    // up) rather than actually queued -- callers that need to know
    // whether their event will ever be serviced (the loader's own
    // ack-back-to-a-module mechanism, specifically) check this instead of
    // blindly waiting on a completion flag that may never flip.
    bool enqueue(const InEvent& evt)
    {
        if (is_cleaning_up_.load(std::memory_order_acquire))
            return false;
        LBuffer payload;
        std::memcpy(payload.buf, &evt, sizeof(InEvent));
        payload.written = sizeof(InEvent);
        int retry = 0;
        while (input_ring_->write(0, payload) == UINT64_MAX)
        {
            LMAXSequentialSharedPage::progressiveYield(retry);
        }
        return true;
    }
    void post_completion(WorkResult* result)
    {
        LBuffer payload;
        std::memcpy(payload.buf, &result, sizeof(WorkResult*));
        payload.written = sizeof(WorkResult*);
        int retry = 0;
        while (completion_ring_->write(0, payload) == UINT64_MAX)
            LMAXSequentialSharedPage::progressiveYield(retry);
    }
    LMAXSequentialSharedPage* output_ring() const { return output_ring_; }
    State& state() { return state_; }
};
} // namespace ETCS
#endif // EVENTSTREAM_H__