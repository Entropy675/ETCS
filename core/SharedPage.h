#ifndef SHAREDPAGE_H__
#define SHAREDPAGE_H__
#include "MemoryArena.h"
#include <atomic>
#include <cassert>
#include <cstring>

namespace ETCS
{

struct SharedPage
{
    char*       buffer;
    long long   capacity;
    std::atomic<long long> written;
    std::atomic<bool>      tombstoned;
    uint64_t    reader_rid;

    static SharedPage* allocate(MemoryArena& arena, uint64_t reader_rid)
    {
        long long chunk_size  = arena.getChunkSize();
        long long header_size = static_cast<long long>(
            (sizeof(SharedPage) + alignof(std::max_align_t) - 1)
            & ~(alignof(std::max_align_t) - 1)
        );
        long long buffer_size = chunk_size - header_size;

        void* mem = arena.allocateRaw(chunk_size, chunk_size);
        SharedPage* page      = new (mem) SharedPage{};
        page->buffer          = static_cast<char*>(mem) + header_size;
        page->capacity        = buffer_size;
        page->written.store(0, std::memory_order_relaxed);
        page->tombstoned.store(false, std::memory_order_relaxed);
        page->reader_rid      = reader_rid;
        return page;
    }

    char* acquireWrite(long long size)
    {
        assert(!tombstoned.load(std::memory_order_relaxed) &&
               "acquireWrite on tombstoned SharedPage — causal violation");

        long long offset = written.fetch_add(size, std::memory_order_seq_cst);
        if (offset + size > capacity)
        {
            written.fetch_sub(size, std::memory_order_seq_cst);
            return nullptr;
        }
        return buffer + offset;
    }

    const char* acquireRead(long long& out_len) const
    {
        out_len = written.load(std::memory_order_acquire);
        return buffer;
    }

    bool isFull() const
    {
        return written.load(std::memory_order_relaxed) >= capacity;
    }

    void tombstone()
    {
        tombstoned.store(true, std::memory_order_release);
    }

    void reset()
    {
        assert(!tombstoned.load(std::memory_order_relaxed) &&
               "reset on tombstoned SharedPage — already deleted");
        written.store(0, std::memory_order_relaxed);
    }
};

// --- SequentialFrame — tagged frame header prepended to each write ---
// Physical layout in buffer: [SequentialFrame][payload bytes]
// Frames are written at arbitrary offsets but read in sequence number order.

struct alignas(8) SequentialFrame
{
    uint64_t  sequence;    // monotonic sequence number — assigned at write time
    uint64_t  writer_rid;  // producing entity's RID
    long long payload_size; // bytes immediately following this header
    std::atomic<bool> ready;   // set last by producer after payload is written
    // payload follows contiguously in buffer — no pointer, no indirection
};

// SequentialSharedPage — same isolated chunk guarantee as SharedPage,
// but each write is a tagged SequentialFrame. The consumer reads frames
// in sequence number order regardless of physical arrival order.
//
// Producers race on two atomics:
//   cursor_   — physical write position, claimed via fetch_add (same as SharedPage)
//   sequence_ — logical sequence number, claimed via fetch_add independently
//
// These two claims are independent — a producer may claim sequence N but not
// yet have filled the buffer at its cursor offset. The consumer must therefore
// scan for the frame whose sequence matches the next expected value rather than
// reading linearly, since a lower-sequence producer may be slow to fill its slot.
//
// This is the same invariant as the LMAX disruptor's claim/publish split.
//
// INVARIANT: backing memory must be zero-initialized before first use.
// SequentialFrame::ready == 0 == false for unwritten slots is load-bearing
// for consumer scan safety. Satisfied by MAP_ANONYMOUS allocation in
// MemoryArena. Do not use with non-zeroing allocators.

struct SequentialSharedPage
{
    char*                  buffer;
    long long              capacity;
    std::atomic<long long> cursor_;    // physical write cursor
    std::atomic<uint64_t>  sequence_;  // next sequence number to assign
    std::atomic<bool>      tombstoned;
    uint64_t               reader_rid;

    static SequentialSharedPage* allocate(MemoryArena& arena, uint64_t reader_rid)
    {
        long long chunk_size  = arena.getChunkSize();
        long long header_size = static_cast<long long>(
            (sizeof(SequentialSharedPage) + alignof(std::max_align_t) - 1)
            & ~(alignof(std::max_align_t) - 1)
        );
        long long buffer_size = chunk_size - header_size;

        void* mem                    = arena.allocateRaw(chunk_size, chunk_size);
        SequentialSharedPage* page   = new (mem) SequentialSharedPage{};
        page->buffer                 = static_cast<char*>(mem) + header_size;
        page->capacity               = buffer_size;
        page->cursor_.store(0,  std::memory_order_relaxed);
        page->sequence_.store(0, std::memory_order_relaxed);
        page->tombstoned.store(false, std::memory_order_relaxed);
        page->reader_rid             = reader_rid;
        return page;
    }

    // Claim a slot and write frame header + payload atomically from this producer's
    // perspective. Returns the sequence number assigned, or UINT64_MAX on failure.
    uint64_t write(uint64_t writer_rid, const char* payload, long long payload_size)
    {
        assert(!tombstoned.load(std::memory_order_relaxed) &&
               "write on tombstoned SequentialSharedPage — causal violation");

        long long frame_size = static_cast<long long>(sizeof(SequentialFrame)) + payload_size;

        // Claim physical slot
        long long offset = cursor_.fetch_add(frame_size, std::memory_order_seq_cst);
        if (offset + frame_size > capacity)
        {
            cursor_.fetch_sub(frame_size, std::memory_order_seq_cst);
            return UINT64_MAX; // page full
        }

        // Claim sequence number — independent of physical slot
        uint64_t seq = sequence_.fetch_add(1, std::memory_order_seq_cst);

        // Write frame header then payload into claimed slot
        SequentialFrame* frame = reinterpret_cast<SequentialFrame*>(buffer + offset);
        frame->sequence        = seq;
        frame->writer_rid      = writer_rid;
        frame->payload_size    = payload_size;
        frame->ready.store(false, std::memory_order_relaxed);
        std::memcpy(buffer + offset + sizeof(SequentialFrame), payload, static_cast<size_t>(payload_size));
        
        // Release store — consumer's acquire scan will see completed frame
        frame->ready.store(true, std::memory_order_release);
        return seq;
    }

    // Scan for the frame with the given sequence number.
    // Returns pointer to payload and fills out_size, or nullptr if not yet visible.
    // Consumer calls this in a spin loop incrementing expected_seq each time.
    //
    // Note: this is O(n) over written frames — acceptable for the page sizes
    // in this system since the chunk is small and cache-resident. If this becomes
    // a bottleneck an index can be layered on top without changing the page layout.
    const char* acquireRead(uint64_t expected_seq, long long& out_size) const
    {
        assert(!tombstoned.load(std::memory_order_relaxed) &&
               "acquireRead on tombstoned SequentialSharedPage — causal violation");

        long long physical_end = cursor_.load(std::memory_order_acquire);
        long long offset       = 0;

        while (offset + static_cast<long long>(sizeof(SequentialFrame)) <= physical_end)
        {
            const SequentialFrame* frame =
                reinterpret_cast<const SequentialFrame*>(buffer + offset);

            // Frame header may be partially written — check ready flag first
            if (!frame->ready.load(std::memory_order_acquire))
                return nullptr; // producer hasn't finished writing this slot yet

            if (frame->sequence == expected_seq)
            {
                out_size = frame->payload_size;
                return buffer + offset + sizeof(SequentialFrame);
            }

            offset += static_cast<long long>(sizeof(SequentialFrame)) + frame->payload_size;
        }

        return nullptr;
    }
    
    // Total number of frames committed so far
    uint64_t frameCount() const
    {
        return sequence_.load(std::memory_order_acquire);
    }

    bool isFull() const
    {
        return cursor_.load(std::memory_order_relaxed) >= capacity;
    }

    void tombstone()
    {
        tombstoned.store(true, std::memory_order_release);
    }

    void reset()
    {
        assert(!tombstoned.load(std::memory_order_relaxed) &&
               "reset on tombstoned SequentialSharedPage — already deleted");
        cursor_.store(0,  std::memory_order_relaxed);
        sequence_.store(0, std::memory_order_relaxed);
    }
};

} // namespace ETCS

#endif
