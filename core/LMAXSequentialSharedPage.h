#ifndef LMAXSEQUENTIALSHAREDPAGE_H__
#define LMAXSEQUENTIALSHAREDPAGE_H__

#include "MemoryArena.h"
#include "SharedPage.h"
#include "Buffer.h"

#include <atomic>
#include <cassert>
#include <cstring>
#include <cstdint>
#include <new>
#include <thread>
#include <chrono>
#include <type_traits>

// ---------------------------------------------------------------------------
// ETCS_CPU_RELAX — portable spin-loop hint.
//
// x86/x64 : PAUSE   (~140 cycles on Skylake+, de-pipelines the spin loop)
// aarch64 : YIELD   (architecturally a hint; a NOP on Cortex-A76/Pi 5)
// other   : no-op
//
// The inline asm form is used on the GCC/Clang ARM path rather than __yield()
// from <arm_acle.h>: same instruction, no header availability question, and it
// encodes as a NOP on pre-ARMv6K. The "memory" clobber is load-bearing — it
// stops the compiler hoisting the watched atomic load out of the spin loop.
//
// NOTE: this properly belongs in core_defs.h — ThreadPool and every other
// spinning site wants the same macro. It is defined here (guarded) only so
// this header stays self-contained; moving it up is then a no-op.
// ---------------------------------------------------------------------------
#ifndef ETCS_CPU_RELAX
  #if defined(_MSC_VER)
    #include <intrin.h>
    #if defined(_M_ARM) || defined(_M_ARM64)
      #define ETCS_CPU_RELAX() __yield()
    #else
      #define ETCS_CPU_RELAX() _mm_pause()
    #endif
  #elif defined(__x86_64__) || defined(__i386__)
    #include <xmmintrin.h>
    #define ETCS_CPU_RELAX() _mm_pause()
  #elif defined(__aarch64__) || defined(__arm__)
    #define ETCS_CPU_RELAX() __asm__ __volatile__("yield" ::: "memory")
  #else
    #define ETCS_CPU_RELAX() ((void)0)
  #endif
#endif

// ---------------------------------------------------------------------------
// Backoff thresholds.
//
// These are NOT arch-portable constants. On x86 each PAUSE burns real cycles,
// so a 64-iteration hint stage is a meaningful backoff. On aarch64 YIELD is
// effectively free, so the same count is a near-tight spin — more interconnect
// traffic, less actual waiting. Thresholds are scaled down accordingly.
// ---------------------------------------------------------------------------
#ifndef ETCS_SPIN_HINT_LIMIT
  #if defined(__aarch64__) || defined(__arm__) || defined(_M_ARM) || defined(_M_ARM64)
    #define ETCS_SPIN_HINT_LIMIT     8
    #define ETCS_SPIN_YIELD_LIMIT   32
  #else
    #define ETCS_SPIN_HINT_LIMIT    64
    #define ETCS_SPIN_YIELD_LIMIT  100
  #endif
#endif

#define ETCS_SPIN_SLEEP_MAX_SHIFT 10   // 1024us ceiling

namespace ETCS
{
struct LMAXSequentialSharedPage
{
    // -----------------------------------------------------------------------
    // Slot geometry
    //
    // A slot is [ SequentialFrame | LBuffer ]. Both sub-objects are reached by
    // pointer arithmetic from the slot base, so the stride must preserve the
    // alignment of whichever sub-object is stricter. Unrounded arithmetic
    // happens to work today (LBuffer is alignas(8), SequentialFrame's members
    // are 8-byte), but "happens to work" is the wrong guarantee here: on x86 a
    // misaligned slot degrades to a slow load, while on aarch64 LDXR/STXR
    // require natural alignment and fault with SIGBUS. Rounding makes the
    // property structural instead of incidental.
    // -----------------------------------------------------------------------
    static const long long PAGE_ALIGN;
    static constexpr long long SLOT_ALIGN =
        static_cast<long long>(alignof(SequentialFrame) > alignof(LBuffer)
                                   ? alignof(SequentialFrame)
                                   : alignof(LBuffer));

    static constexpr long long SLOT_SIZE =
        (static_cast<long long>(sizeof(SequentialFrame) + sizeof(LBuffer))
         + SLOT_ALIGN - 1) & ~(SLOT_ALIGN - 1);

    static_assert(sizeof(SequentialFrame) % alignof(LBuffer) == 0,
                  "LBuffer sub-object would be misaligned within the slot");
    static_assert(SLOT_SIZE % SLOT_ALIGN == 0,
                  "Slot stride does not preserve sub-object alignment");

    // The ring memcpys LBuffer across the ABI boundary. If TBuffer ever gains
    // a non-trivial member this assert fires rather than the copy silently
    // ceasing to be a memcpy. (The broader assert covering Buffer/BBuffer/
    // MBuffer belongs next to the alias declarations in Buffer.h.)
    static_assert(std::is_trivially_copyable_v<LBuffer>,
                  "LBuffer must stay trivially copyable — it crosses DSO and ring boundaries");

    char*                  buffer;
    long long              slot_count_;
    long long              index_mask_;

    alignas(64) std::atomic<uint64_t>  sequence_;
    alignas(64) std::atomic<uint64_t>  publisher_cursor_;
    alignas(64) std::atomic<uint64_t>  consumer_cursor_;

    std::atomic<bool>      tombstoned;
    uint64_t               reader_rid;
    
    static constexpr long long headerSize()
    {
        // Rounded to SLOT_ALIGN so that (mem + header_size) is slot-aligned.
        // PAGE_ALIGN is a multiple of SLOT_ALIGN (both powers of two), so an
        // allocation aligned to PAGE_ALIGN keeps that property.
        return (static_cast<long long>(sizeof(LMAXSequentialSharedPage))
                + SLOT_ALIGN - 1) & ~(SLOT_ALIGN - 1);
    }

    static LMAXSequentialSharedPage* allocate(
        MemoryArena& arena,
        uint64_t     reader_rid,
        long long    slot_count)
    {
        assert(slot_count > 0 && (slot_count & (slot_count - 1)) == 0);

        const long long header_size  = headerSize();
        const long long buffer_bytes = SLOT_SIZE * slot_count;
        const long long total        = header_size + buffer_bytes;

        void* mem = arena.allocateRaw(total, static_cast<size_t>(PAGE_ALIGN));
        LMAXSequentialSharedPage* page = new (mem) LMAXSequentialSharedPage{};

        page->buffer              = static_cast<char*>(mem) + header_size;
        page->slot_count_         = slot_count;
        page->index_mask_         = slot_count - 1;

        // ---------------------------------------------------------------
        // Construct every slot.
        //
        // Without this, the slot region is raw allocateRaw bytes: no
        // SequentialFrame and no LBuffer object exists there, so
        // frame->ready.load() and *dest = payload are both UB, and
        // tryAdvancePublisher reads frame->ready on slot 0 before anything
        // has ever been written to it.
        //
        // This was benign while the arena handed out fresh zeroed mmap
        // pages. With entity outer-shell reclamation and the free-list, a
        // ring can now land on recycled memory, where `ready` reads as
        // garbage-true and the `frame->sequence != next` lap guard is the
        // only thing left standing between that and a bad publish.
        //
        // Constructing LBuffer (not just the frame) also settles the
        // lifetime question in write(): LBuffer is trivially COPYABLE, but
        // its memset ctor makes it non-trivially-default-constructible, and
        // C++17 has no implicit object creation (P0593 is C++20). With the
        // object live, `*dest = payload` is an ordinary assignment.
        //
        // Side benefit for the SandboxGuard determinism goal: slot contents
        // become a function of the program rather than of whatever the arena
        // last held.
        // ---------------------------------------------------------------
        for (long long i = 0; i < slot_count; ++i)
        {
            char* slot = page->buffer + i * SLOT_SIZE;
            new (slot) SequentialFrame{};
            new (slot + sizeof(SequentialFrame)) LBuffer{};
        }

        page->sequence_.store(0, std::memory_order_relaxed);
        page->publisher_cursor_.store(UINT64_MAX, std::memory_order_relaxed);
        page->consumer_cursor_.store(0, std::memory_order_relaxed);

        page->tombstoned.store(false, std::memory_order_relaxed);
        page->reader_rid = reader_rid;

        return page;
    }

    uint64_t write(uint64_t writer_rid, const LBuffer& payload)
    {
        assert(!tombstoned.load(std::memory_order_relaxed));

        uint64_t seq;
        uint64_t consumer;

        while (true)
        {
            seq      = sequence_.load(std::memory_order_relaxed);
            consumer = consumer_cursor_.load(std::memory_order_acquire);

            if (seq - consumer >= static_cast<uint64_t>(slot_count_))
                return UINT64_MAX;

            if (sequence_.compare_exchange_weak(
                    seq,
                    seq + 1,
                    std::memory_order_acq_rel,
                    std::memory_order_relaxed))
            {
                break;
            }
        }

        long long slot_index = static_cast<long long>(seq & index_mask_);
        char* slot = buffer + slot_index * SLOT_SIZE;

        SequentialFrame* frame = reinterpret_cast<SequentialFrame*>(slot);

        // Clear the ready flag FIRST, then fill the fields, then release.
        //
        // The original ordering wrote frame->sequence and friends before
        // re-clearing ready. markConsumed() had already cleared it, so this
        // worked — but only incidentally, and it left a window where the
        // fields were new while the flag had not been re-cleared, with the
        // lap guard as the sole defence. Clearing first makes the invariant
        // structural: ready==true implies every field below is visible.
        frame->ready.store(false, std::memory_order_relaxed);

        frame->sequence     = seq;
        frame->writer_rid   = writer_rid;
        frame->payload_size = static_cast<long long>(payload.written);

        LBuffer* dest = reinterpret_cast<LBuffer*>(slot + sizeof(SequentialFrame));
        copyPayload(dest, payload);

        frame->ready.store(true, std::memory_order_release);

        tryAdvancePublisher(seq);
        return seq;
    }

    const LBuffer* acquireRead(uint64_t expected_seq) const
    {
        uint64_t pub = publisher_cursor_.load(std::memory_order_acquire);

        // NOTE: plain comparison, not isSequenceAhead(). Correct given the
        // UINT64_MAX sentinel is screened on the same line, and uint64 seq
        // wraparound is not a practical concern. isSequenceAhead() below is
        // currently unused — either route this through it or drop it.
        if (pub < expected_seq || pub == UINT64_MAX)
            return nullptr;

        long long slot_index = static_cast<long long>(expected_seq & index_mask_);
        const char* slot = buffer + slot_index * SLOT_SIZE;

        const SequentialFrame* frame =
            reinterpret_cast<const SequentialFrame*>(slot);

        if (frame->sequence != expected_seq)
            return nullptr;

        return reinterpret_cast<const LBuffer*>(slot + sizeof(SequentialFrame));
    }

    // Single-consumer. consumer_cursor_ is advanced by a bare store, so a
    // second attached consumer would clobber rather than contend — this is
    // MPSC by construction, and fails silently if that assumption breaks.
    void markConsumed(uint64_t seq)
    {
        long long slot_index = static_cast<long long>(seq & index_mask_);

        SequentialFrame* frame =
            reinterpret_cast<SequentialFrame*>(buffer + slot_index * SLOT_SIZE);

        frame->ready.store(false, std::memory_order_relaxed);

        consumer_cursor_.store(seq + 1, std::memory_order_release);
    }

    bool isFull() const
    {
        uint64_t seq      = sequence_.load(std::memory_order_relaxed);
        uint64_t consumer = consumer_cursor_.load(std::memory_order_acquire);

        return (seq - consumer) >= static_cast<uint64_t>(slot_count_);
    }

    uint64_t frameCount() const
    {
        return sequence_.load(std::memory_order_acquire);
    }

    uint64_t publishedUpTo() const
    {
        return publisher_cursor_.load(std::memory_order_acquire);
    }

    void tombstone()
    {
        tombstoned.store(true, std::memory_order_release);
    }

    // shutdown() and tombstone() were byte-identical — a false difference
    // between causally identical operations. Kept as a forwarder so existing
    // call sites still compile; collapse the call sites and delete it.
    void shutdown()
    {
        tombstone();
    }

    void reset()
    {
        assert(!tombstoned.load(std::memory_order_relaxed));

        sequence_.store(0, std::memory_order_relaxed);
        publisher_cursor_.store(UINT64_MAX, std::memory_order_relaxed);
        consumer_cursor_.store(0, std::memory_order_relaxed);
    }

    // -----------------------------------------------------------------------
    // Spin strategies
    // -----------------------------------------------------------------------

    // Default: hardware hint, then fall back to the scheduler.
    static inline void relax(int& counter) noexcept
    {
        if (counter < ETCS_SPIN_HINT_LIMIT)
            ETCS_CPU_RELAX();
        else
            std::this_thread::yield();
        counter++;
    }

    static inline void busySpin(int& counter) noexcept
    {
        ETCS_CPU_RELAX();
        counter++;
    }

    // For systems where power savings or core sharing matters.
    //
    // On aarch64 the real idiom for this stage is WFE after an LDXR on the
    // watched address — the core actually sleeps until the exclusive monitor
    // is cleared, rather than spinning on a NOP. Worth doing if the Pi is ever
    // a deployment target rather than a test target.
    static inline void progressiveYield(int& counter) noexcept
    {
        if (counter < ETCS_SPIN_HINT_LIMIT / 4)
            ETCS_CPU_RELAX();
        else if (counter < ETCS_SPIN_YIELD_LIMIT)
            std::this_thread::yield();
        else
        {
            // Doubles every 64 sleeps, capped. Below the ~50us default timer
            // slack the requested duration is fiction anyway.
            const int over  = counter - ETCS_SPIN_YIELD_LIMIT;
            int       shift = 0;
            for (int n = over >> 6; n > 0 && shift < ETCS_SPIN_SLEEP_MAX_SHIFT; n >>= 1) ++shift;
            std::this_thread::sleep_for(std::chrono::microseconds(1 << shift));
        }
        if (counter < (1 << 30)) ++counter;   // saturate; ++ past INT_MAX is UB
    }

    // -----------------------------------------------------------------------
    // Sizing helpers
    // -----------------------------------------------------------------------

    static constexpr long long calculateRequiredMemory(long long slot_count)
    {
        return headerSize() + SLOT_SIZE * slot_count;
    }

    static constexpr long long roundUpToPowerOfTwo(long long v)
    {
        if (v <= 0) return 1;
        v--;
        v |= v >> 1;
        v |= v >> 2;
        v |= v >> 4;
        v |= v >> 8;
        v |= v >> 16;
        v |= v >> 32;
        v++;
        return v;
    }

    static constexpr uint64_t calculateBatchSize(uint64_t expected, uint64_t available)
    {
        return (available >= expected) ? (available - expected) : 0;
    }

    static constexpr bool isSequenceAhead(uint64_t seq_a, uint64_t seq_b)
    {
        return static_cast<int64_t>(seq_a - seq_b) > 0;
    }

private:

    // Width-aware payload copy.
    //
    // `*dest = payload` copies all N bytes of the LBuffer regardless of how
    // many are live. Two problems, both worse on a Pi 5 than on x86:
    //
    //   1. Bandwidth scales with capacity, not message size — even though
    //      frame->payload_size already records the real length.
    //   2. The bytes between `written` and N come along too. reset() only
    //      writes buf[0], so a producer that resets and refills with a short
    //      payload publishes the tail of its PREVIOUS message into shared
    //      memory. Under the marketplace model a third-party signed module
    //      and a core module can sit on opposite ends of the same ring, which
    //      makes that a cross-module read of data that was never payload.
    //      (Same reasoning as the comment on TBuffer::clear().)
    //
    // Residual bytes in the slot after this call are prior RING contents, not
    // prior PRODUCER contents — same trust domain as the ring itself.
    static inline void copyPayload(LBuffer* dest, const LBuffer& src) noexcept
    {
        const size_t n = src.written < LBuffer::bufsize ? src.written
                                                        : LBuffer::bufsize;
        std::memcpy(dest->buf, src.buf, n);
        if (n < LBuffer::bufsize) dest->buf[n] = '\0';

        dest->written     = n;
        dest->read_offset = src.read_offset <= n ? src.read_offset : n;
    }

    void tryAdvancePublisher(uint64_t my_seq)
    {
        // Fast path: if the cursor isn't immediately behind us,
        // we're not the blocker — bail and let whoever holds
        // the gap sequence do the sweep when they publish.
        uint64_t current = publisher_cursor_.load(std::memory_order_acquire);

        if (current != my_seq && current + 1 != my_seq)
        {
            uint64_t next = current + 1;
            long long slot_index = static_cast<long long>(
                next & static_cast<uint64_t>(index_mask_));
            const SequentialFrame* frame = reinterpret_cast<const SequentialFrame*>(
                buffer + slot_index * SLOT_SIZE);

            if (!frame->ready.load(std::memory_order_acquire) ||
                 frame->sequence != next)
                return; // gap still open, not our problem
        }

        // Walk forward from current as long as slots are ready.
        //
        // The happens-before chain here does hold on a weak memory model:
        // producer B's acquire-load of A's frame->ready synchronises with A's
        // release-store, and B's subsequent release-CAS on publisher_cursor_
        // gives the consumer transitive visibility of A's field writes. This
        // part is correct by construction, not by x86's TSO.
        while (true)
        {
            uint64_t cur  = publisher_cursor_.load(std::memory_order_relaxed);
            uint64_t next = cur + 1;

            long long slot_index = static_cast<long long>(
                next & static_cast<uint64_t>(index_mask_));
            const SequentialFrame* frame = reinterpret_cast<const SequentialFrame*>(
                buffer + slot_index * SLOT_SIZE);

            if (!frame->ready.load(std::memory_order_acquire))
                return; // gap — whoever fills this will advance past us

            if (frame->sequence != next)
                return; // lap guard

            if (!publisher_cursor_.compare_exchange_weak(
                    cur, next,
                    std::memory_order_release,
                    std::memory_order_relaxed))
                continue;

            // Advanced — keep walking. This is the sweep that covers all the
            // fast producers who published while we were the blocker.
        }
    }
}; 


inline constexpr long long LMAXSequentialSharedPage::PAGE_ALIGN =
    static_cast<long long>(alignof(LMAXSequentialSharedPage)) > SLOT_ALIGN
        ? static_cast<long long>(alignof(LMAXSequentialSharedPage))
        : SLOT_ALIGN;
static_assert(alignof(LMAXSequentialSharedPage) >= 64,
    "cacheline separation on the alignas(64) cursors requires 64-byte page alignment");
    
// uses compile time tuning parameters to auto allocate slot size
#include "../LMAXAutotuneParams.h"
#ifdef ETCS_AUTOTUNE_DEFS
inline LMAXSequentialSharedPage* allocateTunedRing(MemoryArena& arena, uint64_t rid, int producer_count)
{
    long long slots = producer_count > ETCS_RING_LARGE_THRESHOLD
        ? ETCS_RING_LARGE_SLOTS
        : ETCS_RING_SMALL_SLOTS;

    return LMAXSequentialSharedPage::allocate(arena, rid, slots);
}
#endif

} // namespace ETCS

#endif
