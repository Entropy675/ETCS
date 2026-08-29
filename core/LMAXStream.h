#ifndef LMAXSTREAM_H__
#define LMAXSTREAM_H__

#include "LMAXSequentialSharedPage.h"
#include "MemoryArena.h"
#include "Buffer.h"

#include <cstdint>
#include <functional>

namespace ETCS
{

// ---------------------------------------------------------------------------
// LMAXStream
//
// Arena-backed owner of an LMAXSequentialSharedPage lifecycle.
// The caller never touches sequence numbers, acquireRead, or markConsumed.
// All protocol state (expected_seq_) is internal.
//
// Write surface  : write(writer_rid, payload) → bool
// Consume surface: consume(cb)         — one frame attempt
//                  consumeAll(cb)      — drain until empty
//                  consumeBlocking(cb) — spin with built-in progressive yield
//
// Callback signature: void(const LBuffer&)
//
// Thread model: LMAXStream itself is NOT thread-safe across consume methods.
// Multiple producer threads may call write() concurrently (same guarantee as the
// underlying page). A single thread must then be responsible consumption.
// ---------------------------------------------------------------------------

template <typename Callback = std::function<void(const LBuffer&)>>
struct LMAXStream
{
    // ------------------------------------------------------------------
    // Construction / destruction
    // ------------------------------------------------------------------

    LMAXStream(MemoryArena& arena,
               uint64_t     reader_rid,
               int          producer_count)
        : arena_(arena)
        , page_(allocateTunedRing(arena, reader_rid, producer_count))
        , expected_seq_(0)
    {}

    ~LMAXStream()
    {
        if (page_)
            page_->tombstone();
    }

    // Non-copyable; move is intentionally omitted — the page pointer is
    // arena-backed and should not migrate between owners.
    LMAXStream(const LMAXStream&)            = delete;
    LMAXStream& operator=(const LMAXStream&) = delete;

    // ------------------------------------------------------------------
    // Lifecycle
    // ------------------------------------------------------------------

    // Reset the stream for reuse. Resets the underlying page and rewinds
    // the internal sequence cursor to 0.
    void reset()
    {
        page_->reset();
        expected_seq_ = 0;
    }

    // ------------------------------------------------------------------
    // Write surface
    // ------------------------------------------------------------------

    // Returns true if the write succeeded, false if the ring is full.
    // Sequence numbers are internal — callers never see them.
    bool write(uint64_t writer_rid, const LBuffer& payload)
    {
        return page_->write(writer_rid, payload) != UINT64_MAX;
    }

    // ------------------------------------------------------------------
    // Consume surface
    // ------------------------------------------------------------------

    // Attempt to consume the next expected frame.
    // Calls cb(buffer) if a frame is available, advances the internal
    // cursor, and returns true. Returns false if no frame is ready yet.
    bool consume(Callback&& cb)
    {
        const LBuffer* buf = page_->acquireRead(expected_seq_);
        if (!buf) return false;

        cb(*buf);
        page_->markConsumed(expected_seq_);
        ++expected_seq_;
        return true;
    }

    // Drain all currently available frames in sequence order.
    // Returns the number of frames consumed.
    uint64_t consumeAll(Callback&& cb)
    {
        uint64_t count = 0;
        while (consume(std::forward<Callback>(cb)))
            ++count;
        return count;
    }

    // Spin until exactly `target` frames have been consumed in total
    // since construction (or last reset). Uses progressive yield internally.
    // Returns when total consumed reaches target or tombstone is observed.
    void consumeBlocking(uint64_t target, Callback&& cb)
    {
        int retry = 0;
        while (expected_seq_ < target)
        {
            if (consume(std::forward<Callback>(cb)))
            {
                retry = 0;
                continue;
            }
            LMAXSequentialSharedPage::progressiveYield(retry);
        }
    }

    // Spin indefinitely, calling cb for every frame that arrives.
    // Returns only when stop_flag becomes true AND the ring is drained
    // up to total_expected frames.
    void consumeLoop(std::atomic<bool>& stop_flag,
                     std::atomic<uint64_t>& total_expected,
                     Callback&& cb)
    {
        int retry = 0;
        while (stop_flag.load(std::memory_order_acquire) == false ||
               expected_seq_ < total_expected.load(std::memory_order_relaxed))
        {
            if (consume(std::forward<Callback>(cb)))
            {
                retry = 0;
                continue;
            }
            LMAXSequentialSharedPage::progressiveYield(retry);
        }
    }

    // ------------------------------------------------------------------
    // Inspection
    // ------------------------------------------------------------------

    bool     isFull()        const { return page_->isFull(); }
    uint64_t framesWritten() const { return page_->frameCount(); }
    uint64_t framesConsumed()const { return expected_seq_; }
    uint64_t publishedUpTo() const { return page_->publishedUpTo(); }
    long long slotCount()    const { return page_->slot_count_; }

private:
    MemoryArena&              arena_;
    LMAXSequentialSharedPage* page_;
    uint64_t                  expected_seq_;
};

// ---------------------------------------------------------------------------
// Deduction guide: allow LMAXStream with a plain lambda without spelling the
// template parameter.
//   auto stream = makeLMAXStream(arena, rid, producers, [](const LBuffer&){});
// ---------------------------------------------------------------------------
template <typename Callback>
LMAXStream<Callback> makeLMAXStream(MemoryArena& arena,
                                    uint64_t     reader_rid,
                                    int          producer_count,
                                    Callback&&   /* cb_hint */ )
{
    return LMAXStream<Callback>(arena, reader_rid, producer_count);
}

} // namespace ETCS

#endif // LMAXSTREAM_H__
